#include "conf.h"
#include <algorithm>
#include <memory>
#include <stdlib.h>

using namespace std;

namespace handy {

static string makeKey(string section, string name) {
    string key = section + "." + name; //拼起来
    // Convert to lower case to make section/name lookups case-insensitive
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);  //全部小写就是 key
    return key;
}

string Conf::get(string section, string name, string default_value) {
    string key = makeKey(section, name);
    auto p = values_.find(key);  //从values_ map里找到key
    return p == values_.end() ? default_value : p->second.back(); //values_.end()就是没找到，如果没找到，就返回default_value，否则返回list最后一个元素
}

list<string> Conf::getStrings(string section, string name) { //拿到配置对应的list,如果没设就是空
    string key = makeKey(section, name);
    auto p = values_.find(key);
    return p == values_.end() ? list<string>() : p->second;
}

long Conf::getInteger(string section, string name, long default_value) {
    string valstr = get(section, name, "");  //拿到当前list最后一个或者空
    const char* value = valstr.c_str();   //转成char*
    char* end;
    // This parses "1234" (decimal) and also "0x4D2" (hex)
    long n = strtol(value, &end, 0); //转成数字
    return end > value ? n : default_value; //可以转成数字，就返回数字，否则返回默认值
}

double Conf::getReal(string section, string name, double default_value) {
    string valstr = get(section, name, "");
    const char* value = valstr.c_str();
    char* end;
    double n = strtod(value, &end);  //可以转成小数
    return end > value ? n : default_value;
}

bool Conf::getBoolean(string section, string name, bool default_value) {
    string valstr = get(section, name, "");
    // Convert to lower case to make string comparisons case-insensitive
    std::transform(valstr.begin(), valstr.end(), valstr.begin(), ::tolower);
    if (valstr == "true" || valstr == "yes" || valstr == "on" || valstr == "1") //几种表示true的方法
        return true;
    else if (valstr == "false" || valstr == "no" || valstr == "off" || valstr == "0")
        return false;
    else
        return default_value;
}

namespace {  //匿名的命名空间 匿名的空间是C++用于替代使用static定义作用域为本编译单元的全局函数或全局变量的一种新的替代方式
    struct LineScanner {   //找line
        char* p;
        int err;
        LineScanner(char* ln): p(ln), err(0) {}
        LineScanner& skipSpaces() {     //忽略前边的空格
            while(!err && *p && isspace(*p)) {
                p++;
            }
            return *this;
        }
        string rstrip(char* s, char* e) { //忽略后边的空格
            while (e > s && isspace(e[-1])) {
                e --;
            }
            return string(s, e);
        }
        int peekChar() { skipSpaces(); return *p; } //find第一个字母
        LineScanner& skip(int i) { p += i; return *this; } //前推几个位置
        LineScanner& match(char c) { skipSpaces(); err = *p++ != c; return *this; }
        string consumeTill(char c) {
            skipSpaces();
            char* e = p;
            while (!err && *e && *e != c) {
                e ++;
            }
            if (*e != c) {  //没找到char c
                err = 1;   //报错
                return "";
            }
            char* s = p;
            p = e;
            return rstrip(s, e);  //找到，把尾巴上的" "干掉
        }
        string consumeTillEnd() {
            skipSpaces();
            char* e = p;
            int wasspace = 0;
            while (!err && *e && *e != ';' && *e != '#') {
                if (wasspace) {
                    break;
                }
                wasspace = isspace(*e);
                e ++;
            }
            char* s = p;
            p = e;
            return rstrip(s, e);
        }

    };
}

int Conf::parse(const string& filename1) {  //分析配置文件
    filename = filename1;
    FILE* file = fopen(filename.c_str(), "r");
    if (!file)
        return -1;
    unique_ptr<FILE, decltype(fclose)*> release2(file, fclose); //指针和指针清除函数， declare type自动找类型
    static const int MAX_LINE = 16 * 1024;
    char* ln = new char[MAX_LINE];
    unique_ptr<char[]> release1(ln); //char[]有默认清除函数，就不用声明了
    int lineno = 0;  //行号
    string section, key;
    int err = 0;
    while (!err && fgets(ln, MAX_LINE, file) != NULL) {  //一行行取到ln里
        lineno++;
        LineScanner ls(ln);  //把取出来的内容赋到ls里。
        int c = ls.peekChar();  //找第一个字母
        if (c == ';' || c == '#' || c == '\0') {  //注释行
            continue;
        } else if (c == '[') {  //[]型配置
            section = ls.skip(1).consumeTill(']'); //拿到section section在[section]
            err = ls.match(']').err;
            key = "";
        } else if (isspace(ln[0])) {  //开头是空格
            /* Non-black line with leading whitespace, treat as continuation
               of previous name's value (as per Python ConfigParser). */
            if (!key.empty()) {
                values_[makeKey(section, key)].push_back(ls.consumeTill('\0'));//做到values_里
            } else {
                err = 1;
            }
        } else {
            LineScanner lsc = ls;
            key = ls.consumeTill('=');  //=前是key
            if (ls.peekChar() == '=') {
                ls.skip(1);
            } else {
                ls = lsc;
                key = ls.consumeTill(':');
                err = ls.match(':').err;
            }
            string value = ls.consumeTillEnd();   //从等号往后是value
            values_[makeKey(section, key)].push_back(value);
        }
    }
    return err ? lineno : 0;  //当err不为0时，返回报错行。

}


}
