#pragma once
#include <string>
namespace adsb {
class MapWindow {
public:
    ~MapWindow();
    void show(const std::string& url);
    void close();
private:
    void* controller_=nullptr;
};
}
