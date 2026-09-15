#include "aviation_jsonl.h"
#include <json.hpp>
#include <mutex>
#include <cerrno>
#ifdef _WIN32
#include <fstream>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool appendAviationJSONL(const std::string& path, const std::string& object) {
    auto parsed=nlohmann::json::parse(object,nullptr,false);
    if(!parsed.is_object() || path.empty()) return false;
    std::string line=parsed.dump(-1,' ',false,nlohmann::json::error_handler_t::replace)+"\n";
    // Shared across channel threads AND module instances, not just one receiver.
    static std::mutex writerMutex;
    std::lock_guard<std::mutex> guard(writerMutex);
#ifdef _WIN32
    std::ofstream file(path,std::ios::binary|std::ios::app);
    file.write(line.data(),line.size()); file.flush();
    return file.good();
#else
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    if(fd<0) return false;
    int lockResult;
    do { lockResult=flock(fd,LOCK_EX); } while(lockResult<0 && errno==EINTR);
    if(lockResult<0) { close(fd); return false; }
    struct stat st{};
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode);
    const off_t start=st.st_size;
    size_t written=0;
    while(ok && written<line.size()) {
        auto n=write(fd,line.data()+written,line.size()-written);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { ok=false; break; }
        written+=static_cast<size_t>(n);
    }
    // Recover a failed partial append while holding the cooperative writer lock.
    if(!ok && written>0) ftruncate(fd,start);
    flock(fd,LOCK_UN); close(fd);
    return ok;
#endif
}
