#include "receiver/backend.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
int main(int argc, char** argv) {
    try {
        if (argc != 6)
            throw std::runtime_error("receiver_verify PROFILE RAW_RGB WIDTH HEIGHT OUTPUT_JSON");
        auto cfg = receiver::load_settings(argv[1]);
        int w = std::stoi(argv[3]), h = std::stoi(argv[4]);
        if (w < 1 || h < 1 || w > 1024 || h > 1024)
            throw std::runtime_error("Invalid dimensions");
        receiver::Frame frame;
        frame.header.width = uint16_t(w);
        frame.header.height = uint16_t(h);
        frame.header.format = 1;
        frame.header.bytes = uint32_t(w * h * 3);
        std::ifstream f(argv[2], std::ios::binary);
        f.read(reinterpret_cast<char*>(frame.pixels.data()), frame.header.bytes);
        if (!f || f.peek() != EOF)
            throw std::runtime_error("Raw file size mismatch");
        auto backend = receiver::make_backend(cfg);
        auto result = backend->run(frame);
        auto detections = receiver::decode_yolo(result.output, result.candidates, result.classes, w, h,
                                                cfg.input_size, cfg);
        nlohmann::json j;
        j["shape"] = {1, result.classes + 4, result.candidates};
        j["raw"] = std::vector<float>(result.output.begin(), result.output.end());
        j["detections"] = nlohmann::json::array();
        for (auto& d : detections)
            j["detections"].push_back({d.x, d.y, d.w, d.h, d.score, d.cls});
        std::ofstream out(argv[5]);
        out << j;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
