#include "codec.h"

using namespace std;

namespace handy {

int LineCodec::tryDecode(Slice data, Slice& msg) {
    if (data.size() == 1 && data[0] == 0x04) { //1个0x04的msg，直接return，特定消息
        msg = data;
        return 1;
    }
    for (size_t i = 0; i < data.size(); i ++) {
        if (data[i] == '\n') {              //找到一行
            if (i > 0 && data[i-1] == '\r') {
                msg = Slice(data.data(), i-1); //msg就是这一行
                return i+1;  //返回长度 for example i=2 len= 0,1,2 = 3
            } else {      //linux not \r\n
                msg = Slice(data.data(), i);
                return i+1;
            }
        }
    }
    return 0;
}
void LineCodec::encode(Slice msg, Buffer& buf) {   //buffer是slice拼起来的
    buf.append(msg).append("\r\n");     //用\r\n分割
}

int LengthCodec::tryDecode(Slice data, Slice& msg) {
    if (data.size() < 8) {  //小于8长不解，直接返回 8=mBdT+网络字节序
        return 0;
    }
    int len = net::ntoh(*(int32_t*)(data.data()+4)); //从data.data()+4指向的位置解一个长度出来
    if (len > 1024*1024 || memcmp(data.data(), "mBdT", 4) != 0) { //太长或者不以 mBdt开头，不解
        return -1;
    }
    if ((int)data.size() >= len+8) {  //len后边还有东西，也就是msg内容
        msg = Slice(data.data()+8, len);
        return len+8;
    }
    return 0;
}
void LengthCodec::encode(Slice msg, Buffer& buf) {   //这种加了个前缀，隔开各种slice
    buf.append("mBdT").appendValue(net::hton((int32_t)msg.size())).append(msg);
}      //hton，主机字节序到网络字节序，长4，存msg的长度


}
