#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace helios::test {

class TestClient {
public:
    TestClient();
    ~TestClient();

    bool Connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    void Disconnect();
    void HalfClose();

    ssize_t Send(const std::vector<uint8_t>& data);
    ssize_t Send(const std::string& data);
    ssize_t SendFragmented(const std::string& data, size_t chunk_size, std::chrono::milliseconds delay_between_chunks);

    std::vector<uint8_t> Receive(size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    std::string ReceiveString(size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    
    std::vector<uint8_t> ReceiveExact(size_t exact_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    std::vector<uint8_t> ReceiveUntilClose(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    bool IsConnected() const;

private:
    int fd_{-1};
    bool connected_{false};
};

} // namespace helios::test
