#!/bin/bash
# Cross-builds llama.cpp (volta-nvk) for Android arm64 with the Vulkan backend, statically
# linked shared (libllama, libggml*, libc++_shared) into ~/xval/out/android.
set -eo pipefail
NDK=$HOME/ndk/android-ndk-r27c
SDK=$HOME/xval/sdk/1.4.363.0/x86_64
API=31
SYSLIB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/$API
cd $HOME/xval/android
git log --oneline -1
rm -rf build-android
cmake -G Ninja -B build-android -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared \
  -DBUILD_SHARED_LIBS=ON -DGGML_VULKAN=ON -DGGML_OPENMP=OFF -DGGML_NATIVE=OFF \
  -DGGML_CPU_ARM_ARCH=armv8.2-a+fp16 \
  -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TESTS=ON \
  -DVulkan_INCLUDE_DIR=$SDK/include -DVulkan_LIBRARY=$SYSLIB/libvulkan.so \
  -DVulkan_GLSLC_EXECUTABLE=$SDK/bin/glslc -DSPIRV-Headers_DIR=$SDK/share/cmake/SPIRV-Headers
grep -q "GL_EXT_integer_dot_product supported" build-android/CMakeCache.txt build-android/CMakeFiles/CMakeOutput.log 2>/dev/null || true
cmake --build build-android -j16 --target llama-cli llama-server llama-bench llama-perplexity test-backend-ops
rm -rf $HOME/xval/out/android && mkdir -p $HOME/xval/out/android
cp build-android/bin/{llama-cli,llama-server,llama-bench,llama-perplexity,test-backend-ops} build-android/bin/*.so $HOME/xval/out/android/
cp $NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so $HOME/xval/out/android/
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip $HOME/xval/out/android/*
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -d $HOME/xval/out/android/llama-server $HOME/xval/out/android/libggml-vulkan.so | grep -E "NEEDED|RUNPATH|File"
ls -la $HOME/xval/out/android
echo ANDROID-BUILD-DONE
