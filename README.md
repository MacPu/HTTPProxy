# HTTPProxy
![HTTPProxy Version](https://img.shields.io/badge/version-1.0-blue.svg?style=flat)
![license](https://img.shields.io/badge/license-MIT-brightgreen.svg?style=flat)
![Platform](https://img.shields.io/badge/Platform-iOS | OS X | Android | Linux | Unix-ff69b4.svg?style=flat)


A full-featured http proxy written by C, its can be built both on MacOS, iOS, Android, Linux, Unix

## how to use it

if you want to add this function to your project,  you can use this code

```c
// put 'src/' to your project first.
#include "LAHTTPProxy.h"

start_http_proxy(int port)  // etc:  start_http_proxy(8080);
```

## how to run it

#### Linux and Unix

```
cd linux
make clean; make
./HTTPProxy 8080 # your port

```

#### Xcode (iOS, MacOS)

open Xcode/HTTPProxy.xcodeproj file. and run.

#### Android

JNI built Android.mk
