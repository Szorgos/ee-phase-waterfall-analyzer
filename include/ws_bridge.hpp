#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#include "sha1_base64.hpp"
#include "ringbuffer.hpp"
#include "dsp_pipeline.hpp"

namespace ee_analyzer {

/**
 * @brief Könnyűsúlyú C++ WebSocket híd (RFC 6455)
 * Közvetlen élő adatátviteli csatorna a PipeWire C++ motor és a Webes 3D felület között.
 */
class WebSocketBridge {
public:
    int serverFd = -1;
    int clientFd = -1;
    int port = 8080;
    std::atomic<bool> isRunning{false};
    std::thread serverThread;

    DspPipeline* pipeline = nullptr;
    LockFreeRingBuffer<float>* ringBuffer = nullptr;

    WebSocketBridge(int p = 8080) : port(p) {}

    ~WebSocketBridge() {
        stop();
    }

    void start(DspPipeline* dsp, LockFreeRingBuffer<float>* rb) {
        pipeline = dsp;
        ringBuffer = rb;
        isRunning = true;
        serverThread = std::thread(&WebSocketBridge::runServer, this);
    }

    void stop() {
        if (!isRunning) return;
        isRunning = false;

        if (clientFd >= 0) {
            close(clientFd);
            clientFd = -1;
        }
        if (serverFd >= 0) {
            close(serverFd);
            serverFd = -1;
        }
        if (serverThread.joinable()) {
            serverThread.join();
        }
    }

private:
    void runServer() {
        serverFd = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd < 0) {
            std::cerr << "[WS Bridge] Hiba a socket létrehozásakor!\n";
            return;
        }

        int opt = 1;
        setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "[WS Bridge] Hiba a bind során a " << port << " porton!\n";
            close(serverFd);
            return;
        }

        if (listen(serverFd, 1) < 0) {
            std::cerr << "[WS Bridge] Listen hiba!\n";
            close(serverFd);
            return;
        }

        std::cout << "[WS Bridge] Élő WebSocket híd készen áll a ws://127.0.0.1:" << port << " címen!\n";

        // Beállítjuk non-blocking módba, hogy a leállítás azonnal reagáljon
        fcntl(serverFd, F_SETFL, O_NONBLOCK);

        while (isRunning) {
            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            int newClient = accept(serverFd, (struct sockaddr*)&clientAddr, &addrLen);

            if (newClient >= 0) {
                std::cout << "[WS Bridge] Új grafikus felület kliens csatlakozott!\n";
                if (performHandshake(newClient)) {
                    clientFd = newClient;
                    handleClientCommunication();
                } else {
                    close(newClient);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    bool performHandshake(int sock) {
        char buffer[2048] = {0};
        ssize_t bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) return false;

        std::string request(buffer, bytesRead);
        const std::string keyHeader = "Sec-WebSocket-Key: ";
        size_t keyPos = request.find(keyHeader);
        if (keyPos == std::string::npos) return false;

        size_t keyEnd = request.find("\r\n", keyPos);
        std::string clientKey = request.substr(keyPos + keyHeader.length(), keyEnd - (keyPos + keyHeader.length()));

        // RFC 6455 GUID konkatenálás és SHA1 hash készítés
        const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::vector<uint8_t> sha1Digest = Sha1::hash(clientKey + magicGuid);
        std::string acceptKey = Base64::encode(sha1Digest);

        std::string response = 
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";

        send(sock, response.c_str(), response.length(), 0);
        return true;
    }

    void handleClientCommunication() {
        fcntl(clientFd, F_SETFL, O_NONBLOCK);
        std::vector<float> readChunk(512);

        while (isRunning && clientFd >= 0) {
            // 1. Ringbuffer olvasás és spektrum továbbítás a 3D vízesésnek
            if (ringBuffer) {
                size_t nRead = ringBuffer->read(readChunk.data(), readChunk.size());
                if (nRead > 0) {
                    // Egyszerűsített energia-eloszlás (16 frekvenciasáv) kiszámítása a 3D hálóhoz
                    std::string msg = "{\"type\":\"audio_levels\",\"samples\":[";
                    for (size_t i = 0; i < std::min(nRead, size_t(32)); ++i) {
                        msg += std::to_string(readChunk[i]);
                        if (i + 1 < std::min(nRead, size_t(32))) msg += ",";
                    }
                    msg += "]}";
                    sendWsText(clientFd, msg);
                }
            }

            // 2. Kliens üzenetek (paraméter-állítás) fogadása
            char recvBuf[1024];
            ssize_t n = recv(clientFd, recvBuf, sizeof(recvBuf), 0);
            if (n > 0) {
                // WebSocket frame dekódolás (RFC 6455 unmask)
                if (static_cast<uint8_t>(recvBuf[0]) == 0x81) { // Text frame
                    size_t len = recvBuf[1] & 0x7F;
                    if (len <= 125 && n >= 6) {
                        uint8_t mask[4] = { (uint8_t)recvBuf[2], (uint8_t)recvBuf[3], (uint8_t)recvBuf[4], (uint8_t)recvBuf[5] };
                        std::string payload;
                        for (size_t i = 0; i < len; ++i) {
                            payload.push_back(recvBuf[6 + i] ^ mask[i % 4]);
                        }
                        parseClientMessage(payload);
                    }
                }
            } else if (n == 0) {
                // Kapcsolat bontva
                close(clientFd);
                clientFd = -1;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
        }
    }

    void parseClientMessage(const std::string& msg) {
        // Gyors paraméter-feldolgozás (pl. Preamp vagy Bypass)
        if (msg.find("set_preamp") != std::string::npos && pipeline) {
            size_t valPos = msg.find("\"val\":");
            if (valPos != std::string::npos) {
                double val = std::stod(msg.substr(valPos + 6));
                pipeline->preampGainDb = val;
                std::cout << "[WS Bridge] Preamp frissítve a PipeWire láncban: " << val << " dB\n";
            }
        }
    }

    void sendWsText(int sock, const std::string& text) {
        std::vector<uint8_t> frame;
        frame.push_back(0x81); // Fin + Text opcode
        size_t len = text.size();

        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        }

        frame.insert(frame.end(), text.begin(), text.end());
        send(sock, frame.data(), frame.size(), MSG_NOSIGNAL);
    }
};

} // namespace ee_analyzer
