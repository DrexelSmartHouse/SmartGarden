#include "Arduino.h"
#include "credentials.h"

myCredentials::myCredentials(){
    _SSID="INSERT WIFI NAME";
    _Pwd="INSERT WIFI PASSWORD";
}
myCredentials::~myCredentials(){
    _SSID="";
    _Pwd="";
}
char* myCredentials::ssid(){
    return _SSID;
}

char* myCredentials::pwd(){
    return _Pwd;
}