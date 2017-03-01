#pragma once
#include <string.h>
#include <string>
#include <vector>

namespace handy {

class Slice {
public:
    Slice() : pb_("") { pe_ = pb_; }      //多种构造函数，空构造头尾指针相同
    Slice(const char* b, const char* e):pb_(b), pe_(e) {}
    Slice(const char* d, size_t n) : pb_(d), pe_(d + n) { }
    Slice(const std::string& s) : pb_(s.data()), pe_(s.data() + s.size()) { }
    Slice(const char* s) : pb_(s), pe_(s + strlen(s)) { }

    const char* data() const { return pb_; }       //拿数据
    const char* begin() const { return pb_; }
    const char* end() const { return pe_; }
    char front() { return *pb_; }
    char back() { return pe_[-1]; }          //like python，最后一个字母
    size_t size() const { return pe_ - pb_; }    //大小
    void resize(size_t sz) { pe_ = pb_ + sz; }   //调大小
    inline bool empty() const { return pe_ == pb_; }   //数据置空 inline 直接替换，减少stack构建次数
    void clear() { pe_ = pb_ = ""; }    //清除Slice 估计用于析构

    //return the eated data
    Slice eatWord();  //干掉一个词 ' '
    Slice eatLine();  //干掉一行  '\n'
    Slice eat(int sz) { Slice s(pb_, sz); pb_+=sz; return s; }       //干掉几个char
    Slice sub(int boff, int eoff=0) const { Slice s(*this); s.pb_ += boff; s.pe_ += eoff; return s; }  //头尾同时推
    Slice& trimSpace(); //干掉空格

    inline char operator[](size_t n) const { return pb_[n]; }  //返回第几个字母

    std::string toString() const { return std::string(pb_, pe_); }   //用标准库把Slice变成string
    // Three-way comparison.  Returns value:
    int compare(const Slice& b) const;

    // Return true if "x" is a prefix of "*this"
    bool starts_with(const Slice& x) const {
        return (size() >= x.size() && memcmp(pb_, x.pb_, x.size()) == 0);
    }
    // Return true if "x" is a end of "*this"
    bool end_with(const Slice& x) const {
        return (size() >= x.size() && memcmp(pe_ - x.size(), x.pb_, x.size()) == 0);
    }
    operator std::string() const { return std::string(pb_, pe_); }   //强制转换方法
    std::vector<Slice> split(char ch) const ;   //一个Slice用ch拆成数组
private:
    const char* pb_;               //私有数据成员 pointer to a const char内容不能改， b for begin
    const char* pe_;               //私有数据成员 _用于私有命名, e for end
};

inline Slice Slice::eatWord() {   //find a word
    const char* b = pb_;
    while (b < pe_ && isspace(*b)) {
        b++;
    }
    const char* e = b;
    while (e < pe_ && !isspace(*e)) {
        e ++;
    }
    pb_ = e;
    return Slice(b, e-b);
}

inline Slice Slice::eatLine() {  //find a line
    const char* p = pb_;
    while (pb_<pe_ && *pb_ != '\n' && *pb_!='\r') {
        pb_++;
    }
    return Slice(p, pb_-p);
}

inline Slice& Slice::trimSpace() {   //delete space from begin and end
    while (pb_ < pe_ && isspace(*pb_)) pb_ ++;
    while (pb_ < pe_ && isspace(pe_[-1])) pe_ --;
    return *this;
}

inline bool operator < (const Slice& x, const Slice& y) {   //符号操作，内联
    return x.compare(y) < 0;
}

inline bool operator==(const Slice& x, const Slice& y) {   //等于的判断。数据内容也要一样
    return ((x.size() == y.size()) &&
        (memcmp(x.data(), y.data(), x.size()) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) {  //用上边的函数，good
    return !(x == y);
}

inline int Slice::compare(const Slice& b) const {
    size_t sz = size(), bsz = b.size();
    const int min_len = (sz < bsz) ? sz : bsz;
    int r = memcmp(pb_, b.pb_, min_len);  //先比比最小长度
    if (r == 0) {   //一样
        if (sz < bsz) r = -1;       //谁长谁大，-1代表在前边,+1代表在后边
        else if (sz > bsz) r = +1;
    }
    return r;
}

inline std::vector<Slice> Slice::split(char ch) const {
    std::vector<Slice> r;                 //标准函数库可以给任意对象构造数组。
    const char* pb = pb_;
    for (const char* p = pb_; p < pe_; p++) {
        if (*p == ch) {
            r.push_back(Slice(pb, p));
            pb = p+1;
        }
    }
    if (pe_ != pb_)                  //last one
        r.push_back(Slice(pb, pe_));
    return r;
}

}
