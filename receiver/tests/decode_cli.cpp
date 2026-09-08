#include "receiver/control.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
int main(int argc, char** argv) {
    try {
        if (argc != 7)
            throw std::runtime_error("receiver_decode PROFILE OUTPUT_F32 WIDTH HEIGHT CLASSES CANDIDATES");
        auto settings = receiver::load_settings(argv[1]);
        int width = std::stoi(argv[3]), height = std::stoi(argv[4]), classes = std::stoi(argv[5]),
            count = std::stoi(argv[6]);
        if (width < 1 || width > 1024 || height < 1 || height > 1024 || classes < 1 || classes > 10000 ||
            count < 1 || count > 100000 || size_t(classes + 4) * count > 50'000'000)
            throw std::runtime_error("Invalid tensor dimensions");
        std::vector<float> tensor(size_t(classes + 4) * count);
        std::ifstream f(argv[2], std::ios::binary);
        f.read(reinterpret_cast<char*>(tensor.data()), std::streamsize(tensor.size() * sizeof(float)));
        if (!f || f.peek() != EOF)
            throw std::runtime_error("Output tensor file length mismatch");
        auto detections =
            receiver::decode_yolo(tensor, count, classes, width, height, settings.input_size, settings);
        auto result = nlohmann::json::array();
        for (auto& d : detections)
            result.push_back({d.x, d.y, d.w, d.h, d.score, d.cls});
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
