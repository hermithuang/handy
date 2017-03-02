#include "file.h"
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
using namespace std;

namespace handy {

Status file::getContent(const std::string& filename, std::string& cont){
    int fd = open(filename.c_str(), O_RDONLY); //只读打开
    if (fd < 0) {  //没打开成
        return Status::ioError("open", filename);  //Status类
    }
    ExitCaller ec1([=]{ close(fd); });  //注册一个退出时调用的函数
    char buf[4096];
    for(;;) {
        int r = read(fd, buf, sizeof buf);
        if (r < 0) {
            return Status::ioError("read", filename);
        } else if (r == 0) {
            break;
        }
        cont.append(buf, r);   //拼起来
    }
    return Status();  //完成，返回一个Status
}

Status file::writeContent(const std::string& filename, const std::string& cont) {
    int fd = open(filename.c_str(), O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return Status::ioError("open", filename);
    }
    ExitCaller ec1([=]{ close(fd); });
    int r = write(fd, cont.data(), cont.size());  //写进去
    if (r < 0) {
        return Status::ioError("write", filename);
    }
    return Status();
}

Status file::renameSave(const string& name, const string& tmpName, const string& cont) {
    Status s = writeContent(tmpName, cont);
    if (s.ok()) {  //Status 对
        unlink(name.c_str());  //删了老的
        s = renameFile(tmpName, name);
    }
    return s;
}

Status file::getChildren(const std::string& dir, std::vector<std::string>* result) {
    result->clear();
    DIR* d = opendir(dir.c_str());  //打开一个目录
    if (d == NULL) {
        return Status::ioError("opendir", dir);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {   //不断读目录
        result->push_back(entry->d_name);   //把文件名存到vector里
    }
    closedir(d);
    return Status();
}

Status file::deleteFile(const string& fname) {
    if (unlink(fname.c_str()) != 0) { //删
        return Status::ioError("unlink", fname);
    }
    return Status();
}

Status file::createDir(const std::string& name) {
    if (mkdir(name.c_str(), 0755) != 0) {  //建个目录
        return Status::ioError("mkdir", name);
    }
    return Status();
}

Status file::deleteDir(const std::string& name) {
    if (rmdir(name.c_str()) != 0) {    //删个目录
        return Status::ioError("rmdir", name);
    }
    return Status();
}

Status file::getFileSize(const std::string& fname, uint64_t* size) {
    struct stat sbuf;      //拿个文件大小
    if (stat(fname.c_str(), &sbuf) != 0) {
        *size = 0;
        return Status::ioError("stat", fname);
    } else {
        *size = sbuf.st_size;
    }
    return Status();
}

Status file::renameFile(const std::string& src, const std::string& target) {
    if (rename(src.c_str(), target.c_str()) != 0) {
        return Status::ioError("rename", src + " " + target);
    }    //改个名
    return Status();
}

bool file::fileExists(const std::string& fname) {   //存在
    return access(fname.c_str(), F_OK) == 0;
}

}
