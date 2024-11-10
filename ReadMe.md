# Сборщик мусора для языка Stella
Реализован вариант semi-DFS копирующей сборкой мусора (Algorithm 13.11) с поддержкой сборки по поколениям (§13.4)

## Запуск
Сначала необходимо транслировать программу на Stella в программу на C. Сделать можно несколькими способами: 
- Через docker командой `docker run -i fizruk/stella compile < program.stella > program.c`
- Используя транслятор в зависимости от платформы
- Через сайт https://fizruk.github.io/stella/

Затем необходимо скомпилировать полученную программу  
`gcc -std=c11 program.c stella/runtime.c stella/gc.c -o program`

И запустить ее