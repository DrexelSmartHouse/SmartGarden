#ifndef credentials_h
#define credentials_h

#include "Arduino.h"

class myCredentials{
    public:
        myCredentials();
        ~myCredentials();
        char* ssid();
        char* pwd();

    private:
        char* _SSID;
        char* _Pwd;
};

#endif