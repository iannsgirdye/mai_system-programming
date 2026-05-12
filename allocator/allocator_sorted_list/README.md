# Сборка и запуск

## Тестирование
**Создание сборки**
``` bash
cmake --build build --target sys_prog_allctr_allctr_srtd_lst_tests
```

**Запуск тестирования**
``` bash
./build/allocator/allocator_sorted_list/tests/sys_prog_allctr_allctr_srtd_lst_tests
```


## Первичная настройка среды или устранение неполадок
**Удаление старой сборки** (при необходимости)
``` bash
rm -rf build
```

**Создание новой сборки**
``` bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/mnt/c/Users/user/Programming/utils/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_CXX_STANDARD=20
```


## Примечание
Я использую локальный Google Test, поэтому
1. в [``/CMakeLists.txt``](/CMakeLists.txt)
``` cmake
# Заменил это
include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip)
FetchContent_MakeAvailable(googletest)

# На это
find_package(GTest REQUIRED)
```

2. в [``/allocator/allocator_sorted_list/tests/CMakeLists.txt``](/allocator/allocator_sorted_list/tests/CMakeLists.txt)
``` cmake
# Заменил это
target_link_libraries(
        sys_prog_allctr_allctr_srtd_lst_tests
        PRIVATE
        gtest_main)

# На это
target_link_libraries(
        sys_prog_allctr_allctr_srtd_lst_tests
        PRIVATE
        GTest::gtest_main)
```
