#include "receiver/backend.hpp"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <windows.h>
#include <bcrypt.h>
#include <cuda_runtime.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#if NV_TENSORRT_MAJOR != 10
#error This backend targets TensorRT 10.x. Use the documented SDK; TensorRT 11 requires a separate strongly typed model conversion.
#endif
namespace receiver {
void launch_preprocess(const uint8_t*, float*, int, int, int, int, int, int, int, int, cudaStream_t);
static void checked(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(e));
}
class Log final : public nvinfer1::ILogger {
    std::mutex mutex_;
    std::string error_;

  public:
    std::string message() {
        std::lock_guard lock(mutex_);
        return error_;
    }
    void log(Severity s, const char* message) noexcept override {
        if (s <= Severity::kERROR)
            try {
                std::lock_guard lock(mutex_);
                error_ = message;
            } catch (...) {
            }
    }
};
std::string sha256(std::span<const uint8_t> data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, returned = 0;
    std::array<uint8_t, 32> digest{};
    auto check = [](NTSTATUS s) {
        if (s < 0)
            throw std::runtime_error("SHA-256 failed");
    };
    check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    try {
        check(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                                sizeof(object_size), &returned, 0));
        std::vector<uint8_t> object(object_size);
        check(BCryptCreateHash(alg, &hash, object.data(), object_size, nullptr, 0, 0));
        for (size_t offset = 0; offset < data.size();) {
            auto n = ULONG(std::min<size_t>(data.size() - offset, 1 << 20));
            check(BCryptHashData(hash, const_cast<PUCHAR>(data.data() + offset), n, 0));
            offset += n;
        }
        check(BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0));
        BCryptDestroyHash(hash);
        hash = nullptr;
    } catch (...) {
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw;
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    std::ostringstream out;
    for (auto v : digest)
        out << std::hex << std::setw(2) << std::setfill('0') << int(v);
    return out.str();
}
static std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Cannot open " + path.string());
    auto n = f.tellg();
    if (n <= 0 || n > std::streamoff(1024ull * 1024 * 1024))
        throw std::runtime_error("Invalid/oversized model or engine");
    std::vector<uint8_t> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    if (!f)
        throw std::runtime_error("Incomplete model read");
    return b;
}
class TrtBackend final : public Backend {
    Log log_;
    std::unique_ptr<nvinfer1::IRuntime> runtime_;
    std::unique_ptr<nvinfer1::ICudaEngine> engine_;
    std::unique_ptr<nvinfer1::IExecutionContext> context_;
    cudaStream_t stream_ = nullptr;
    cudaEvent_t begin_ = nullptr, prepared_ = nullptr, inferred_ = nullptr, done_ = nullptr;
    uint8_t* pinned_ = nullptr;
    uint8_t* raw_ = nullptr;
    float* input_ = nullptr;
    float* output_ = nullptr;
    float* host_output_ = nullptr;
    int size_ = 0, classes_ = 0, count_ = 0;
    size_t output_count_ = 0;
    std::string input_name_, output_name_, description_;
    void cleanup() {
        if (stream_)
            cudaStreamSynchronize(stream_);
        if (raw_)
            cudaFree(raw_);
        if (input_)
            cudaFree(input_);
        if (output_)
            cudaFree(output_);
        if (pinned_)
            cudaFreeHost(pinned_);
        if (host_output_)
            cudaFreeHost(host_output_);
        for (auto e : {begin_, prepared_, inferred_, done_})
            if (e)
                cudaEventDestroy(e);
        if (stream_)
            cudaStreamDestroy(stream_);
    }

  public:
    explicit TrtBackend(const Settings& s) {
        try {
            initialize(s);
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~TrtBackend() override {
        cleanup();
    }
    std::string description() const override {
        return description_;
    }
    std::pair<size_t, size_t> device_memory() const override {
        size_t free = 0, total = 0;
        checked(cudaMemGetInfo(&free, &total));
        return {free, total};
    }
    void initialize(const Settings& s) {
        size_ = s.input_size;
        checked(cudaSetDevice(0));
        cudaDeviceProp props{};
        checked(cudaGetDeviceProperties(&props, 0));
        if (props.major < 7)
            throw std::runtime_error("TensorRT FP16 deployment requires a supported NVIDIA GPU");
        auto model = read_file(s.model);
        auto hash = sha256(model);
        const auto metadata = s.metadata.empty() ? s.model + ".json" : s.metadata;
        std::ifstream mf(metadata);
        if (!mf)
            throw std::runtime_error("Missing model manifest. Export with tools/export_model.py");
        nlohmann::json meta;
        mf >> meta;
        if (meta.at("version") != 1 || meta.at("task") != "detect" || meta.at("layout") != "NCHW" ||
            meta.at("output") != "raw_yolo11" || meta.at("sha256") != hash)
            throw std::runtime_error(
                "Model manifest mismatch; only verified raw YOLO11 detection exports are supported");
        classes_ = int(meta.at("names").size());
        if (classes_ < 1 || classes_ > 10000)
            throw std::runtime_error("Invalid model class metadata");
        for (int c : s.classes)
            if (c >= classes_)
                throw std::runtime_error("Selected class is absent from model");
        int driver = 0, runtime_version = 0;
        checked(cudaDriverGetVersion(&driver));
        checked(cudaRuntimeGetVersion(&runtime_version));
        std::string key = hash + ":" + std::to_string(size_) + ":" + props.name + ":" +
                          std::to_string(NV_TENSORRT_VERSION) + ":" + std::to_string(driver) + ":" +
                          std::to_string(runtime_version) + ":fp16-v1:" + std::to_string(classes_);
        auto cache_key = sha256(Bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
        std::filesystem::create_directories("cache");
        auto cache = std::filesystem::path("cache") / (cache_key + ".engine");
        runtime_.reset(nvinfer1::createInferRuntime(log_));
        if (!runtime_)
            throw std::runtime_error("TensorRT runtime creation failed: " + log_.message());
        if (std::filesystem::exists(cache)) {
            auto data = read_file(cache);
            engine_.reset(runtime_->deserializeCudaEngine(data.data(), data.size()));
        }
        if (!engine_) {
            std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(log_));
            if (!builder)
                throw std::runtime_error("TensorRT builder failed");
            std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
            if (!network)
                throw std::runtime_error("TensorRT network creation failed");
            std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, log_));
            if (!parser)
                throw std::runtime_error("ONNX parser creation failed");
            if (!parser->parse(model.data(), model.size()))
                throw std::runtime_error("ONNX parsing failed: " + log_.message());
            if (network->getNbInputs() != 1 || network->getNbOutputs() != 1)
                throw std::runtime_error(
                    "Expected one detection input/output; pose/segmentation/NMS exports are unsupported");
            auto* input = network->getInput(0);
            auto dims = input->getDimensions();
            auto out = network->getOutput(0)->getDimensions();
            if (dims.nbDims != 4 || (dims.d[0] != 1 && dims.d[0] != -1) || dims.d[1] != 3 ||
                (dims.d[2] != -1 && dims.d[2] != size_) || (dims.d[3] != -1 && dims.d[3] != size_) ||
                out.nbDims != 3 || (out.d[1] != 4 + classes_ && out.d[1] != -1) ||
                input->getType() != nvinfer1::DataType::kFLOAT ||
                network->getOutput(0)->getType() != nvinfer1::DataType::kFLOAT)
                throw std::runtime_error("Expected float32 [1,3,H,W] and [1,4+classes,N]; fixed model size "
                                         "must match selected input");
            std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
            config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1024ull * 1024 * 1024);
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
            // Profiles are owned by IBuilder; their destructor is protected.
            auto* profile = builder->createOptimizationProfile();
            if (!profile)
                throw std::runtime_error("Cannot create optimization profile");
            if (dims.d[0] == -1 || dims.d[2] == -1 || dims.d[3] == -1) {
                for (auto selector : {nvinfer1::OptProfileSelector::kMIN, nvinfer1::OptProfileSelector::kOPT,
                                      nvinfer1::OptProfileSelector::kMAX})
                    if (!profile->setDimensions(input->getName(), selector,
                                                nvinfer1::Dims4{1, 3, size_, size_}))
                        throw std::runtime_error("Invalid optimization dimensions");
                if (config->addOptimizationProfile(profile) < 0)
                    throw std::runtime_error("Invalid TensorRT profile");
            }
            std::unique_ptr<nvinfer1::IHostMemory> serialized(
                builder->buildSerializedNetwork(*network, *config));
            if (!serialized)
                throw std::runtime_error("TensorRT engine build failed: " + log_.message());
            engine_.reset(runtime_->deserializeCudaEngine(serialized->data(), serialized->size()));
            if (!engine_)
                throw std::runtime_error("Engine deserialization failed");
            auto tmp = cache;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(static_cast<const char*>(serialized->data()), std::streamsize(serialized->size()));
                if (!f)
                    throw std::runtime_error("Engine cache write failed");
            }
            std::error_code error;
            std::filesystem::remove(cache, error);
            std::filesystem::rename(tmp, cache);
        }
        if (engine_->getNbIOTensors() != 2)
            throw std::runtime_error("Cached engine has an unsupported I/O contract");
        for (int i = 0; i < 2; ++i) {
            const char* name = engine_->getIOTensorName(i);
            if (engine_->getTensorDataType(name) != nvinfer1::DataType::kFLOAT)
                throw std::runtime_error("Engine I/O must be float32");
            if (engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
                input_name_ = name;
            else
                output_name_ = name;
        }
        if (input_name_.empty() || output_name_.empty())
            throw std::runtime_error("Missing engine input/output");
        context_.reset(engine_->createExecutionContext());
        if (!context_ || !context_->setInputShape(input_name_.c_str(), nvinfer1::Dims4{1, 3, size_, size_}))
            throw std::runtime_error("Cannot select model input size");
        auto out = context_->getTensorShape(output_name_.c_str());
        if (out.nbDims != 3 || out.d[0] != 1 || out.d[1] != 4 + classes_ || out.d[2] < 1 || out.d[2] > 100000)
            throw std::runtime_error("Unsupported raw YOLO output shape");
        count_ = int(out.d[2]);
        output_count_ = size_t(classes_ + 4) * count_;
        checked(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
        for (auto* e : {&begin_, &prepared_, &inferred_, &done_})
            checked(cudaEventCreate(e));
        checked(cudaMallocHost(reinterpret_cast<void**>(&pinned_), max_frame_bytes));
        checked(cudaMalloc(reinterpret_cast<void**>(&raw_), max_frame_bytes));
        checked(cudaMalloc(reinterpret_cast<void**>(&input_), size_t(3) * size_ * size_ * sizeof(float)));
        checked(cudaMalloc(reinterpret_cast<void**>(&output_), output_count_ * sizeof(float)));
        checked(cudaMallocHost(reinterpret_cast<void**>(&host_output_), output_count_ * sizeof(float)));
        if (!context_->setTensorAddress(input_name_.c_str(), input_) ||
            !context_->setTensorAddress(output_name_.c_str(), output_))
            throw std::runtime_error("Cannot bind TensorRT tensors");
        description_ = "TensorRT FP16 / " + std::string(props.name) + " / " + std::to_string(size_) + "x" +
                       std::to_string(size_);
        checked(cudaMemsetAsync(input_, 0, size_t(3) * size_ * size_ * sizeof(float), stream_));
        for (int i = 0; i < 10; ++i)
            if (!context_->enqueueV3(stream_))
                throw std::runtime_error("TensorRT warmup failed");
        checked(cudaStreamSynchronize(stream_));
    }
    Inference run(const Frame& frame) override {
        const auto& h = frame.header;
        auto b = letterbox(h.width, h.height, size_);
        auto copy_start = now_ns();
        std::memcpy(pinned_, frame.pixels.data(), h.bytes);
        double host_copy_ms = double(now_ns() - copy_start) / 1e6;
        checked(cudaEventRecord(begin_, stream_));
        checked(cudaMemcpyAsync(raw_, pinned_, h.bytes, cudaMemcpyHostToDevice, stream_));
        launch_preprocess(raw_, input_, h.width, h.height, h.format == 1 ? 3 : 4, size_, b.resized_w,
                          b.resized_h, b.left, b.top, stream_);
        checked(cudaGetLastError());
        checked(cudaEventRecord(prepared_, stream_));
        if (!context_->enqueueV3(stream_))
            throw std::runtime_error("TensorRT inference failed: " + log_.message());
        checked(cudaEventRecord(inferred_, stream_));
        checked(cudaMemcpyAsync(host_output_, output_, output_count_ * sizeof(float), cudaMemcpyDeviceToHost,
                                stream_));
        checked(cudaEventRecord(done_, stream_));
        checked(cudaEventSynchronize(done_));
        float upload = 0, infer = 0, download = 0;
        checked(cudaEventElapsedTime(&upload, begin_, prepared_));
        checked(cudaEventElapsedTime(&infer, prepared_, inferred_));
        checked(cudaEventElapsedTime(&download, inferred_, done_));
        return {{host_output_, output_count_}, count_, classes_, host_copy_ms + upload + download, infer};
    }
};
std::unique_ptr<Backend> make_backend(const Settings& settings) {
    return std::make_unique<TrtBackend>(settings);
}
} // namespace receiver
