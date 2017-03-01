#pragma once          //多重包含时只包含一次头文件
#include "slice.h"
#include "net.h"
namespace handy {     //自己的命名空间

struct CodecBase {
    // > 0 解析出完整消息，消息放在msg中，返回已扫描的字节数     //return value
    // == 0 解析部分消息
    // < 0 解析错误
    virtual int tryDecode(Slice data, Slice& msg) = 0;  //slice看slice.h
    virtual void encode(Slice msg, Buffer& buf) = 0;     //=0纯虚函数
    virtual CodecBase* clone() = 0;
};

//以\r\n结尾的消息
struct LineCodec: public CodecBase{
    int tryDecode(Slice data, Slice& msg) override; //override确保在派生类中声明的重载函数跟基类的虚函数有相同的签名
    void encode(Slice msg, Buffer& buf) override;
    CodecBase* clone() override { return new LineCodec(); }  // a = b.clone();
};

//给出长度的消息
struct LengthCodec:public CodecBase {
    int tryDecode(Slice data, Slice& msg) override; //final阻止类的进一步派生和虚函数的进一步重载
    void encode(Slice msg, Buffer& buf) override;
    CodecBase* clone() override { return new LengthCodec(); }
};

};
