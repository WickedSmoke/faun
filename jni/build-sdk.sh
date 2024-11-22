#!/usr/bin/bash

SDK=/tmp/faun-0.2.0

if [ ! -d /tmp/usr/arm64-v8a ]; then
	tar xjf jni/vorbis-android-static-1.3.7.tar.bz2 -C /tmp
fi

${ANDROID_NDK_ROOT}/ndk-build

rm -rf $SDK
mkdir -p $SDK/include
cp faun.h $SDK/include
cp -a obj/local $SDK/libs
rm -rf $SDK/libs/*/objs
cp /tmp/usr/arm64-v8a/lib/*.a $SDK/libs/arm64-v8a
cp /tmp/usr/armv7a/lib/*.a $SDK/libs/armeabi-v7a
cp /tmp/usr/x86_64/lib/*.a $SDK/libs/x86_64
rm $SDK/libs/*/libvorbisenc.a
#tree $SDK

tar cjf /tmp/faun-android-static-0.2.0.tar.bz2 -C /tmp faun-0.2.0
