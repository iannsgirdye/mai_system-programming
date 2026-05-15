# ЛР по СП в МАИ
Выполнение лабораторных работ по дисциплине "Системное программирование" <br/>
в четвёртом семестре направления "Фундаментальная информатика и информационные технологии" <br/>
в [Московском авиационном институте](https://mai.ru)

- [Лабораторная работа №1](</Tasks/2526 МАИ СП 1.pdf>)
- [Лабораторная работа №2](</Tasks/2526 МАИ СП 2.pdf>)



## Сборка и запуск
### Тестирование
**Создание сборки**
``` bash
cmake --build build --target <test-name>
```

**Запуск тестирования**
``` bash
./build/<part-name>/<task-name>/tests/<test-name>
```

| `<part-name>`           | `<task-name>`             | `<test-name>`                                |
|-------------------------|---------------------------|----------------------------------------------|
| `allocator`             | `allocator_global_heap`   | `sys_prog_allctr_allctr_glbl_hp_tests`       |
| `allocator`             | `allocator_sorted_list`   | `sys_prog_allctr_allctr_srtd_lst_tests`      |
| `allocator`             | `allocator_boundary_tags` | `sys_prog_allctr_allctr_bndr_tgs_tests`      |
| `associative_container` | `indexing_tree_b_tree`    | `sys_prog_assctv_cntnr_indxng_tr_b_tr_tests` |


### Первичная настройка среды или устранение неполадок
**Удаление старой сборки** (при необходимости)
``` bash
rm -rf build
```

**Создание новой сборки**
``` bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=<path-to-repo>/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_CXX_STANDARD=20
```


### Использование локального Google Test
0. Установить и настроить Google Test

1. Необходимо в ``/CMakeLists.txt``
``` cmake
# Заменить это
include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip)
FetchContent_MakeAvailable(googletest)

# На это
find_package(GTest REQUIRED)
```

2. Необходимо в ``/<part-name>/<task-name>/tests/CMakeLists.txt``
``` cmake
# Заменить это
target_link_libraries(... gtest_main)

# На это
target_link_libraries(... GTest::gtest_main)
```
