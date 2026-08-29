/* gui/touch/touch_app.h — контракт приложения тач-оболочки.
 *
 * На десктопе (gui/wm.c) приложения были вшиты в оконный менеджер: у него
 * прямо в коде лежали две переменные term_win/set_win, и добавление
 * третьего приложения означало правку десятка мест в цикле обработки
 * мыши. Здесь приложение — это структура с указателями на функции, а
 * оболочка работает со списком таких структур единообразно. Добавить
 * приложение = написать один .c-файл и вписать его в список в touch_ui.c.
 *
 * Изоляции памяти по-прежнему нет (см. apps/app_api.h): всё линкуется в
 * один образ и живёт в одном адресном пространстве. Это разделение
 * ответственности, а не безопасности.
 */
#ifndef GUI_TOUCH_APP_H
#define GUI_TOUCH_APP_H

#include <stdint.h>

typedef struct {
    const char *name;    /* подпись под иконкой и в статусбаре */
    const char *glyph;   /* 1-3 символа внутри иконки          */
    uint8_t     color;   /* верх градиента иконки               */
    uint8_t     color2;  /* низ градиента — ТЁМНЫЙ ОТТЕНОК ТОГО ЖЕ
                          * цвета. Раньше низ у всех иконок был один
                          * (бирюзовый акцент), и оранжевые настройки
                          * перетекали в бирюзу — выглядело как ошибка. */

    void (*init)(void);
    /* Область, отведённая приложению. Вызывается при запуске и при каждом
     * появлении/исчезновении экранной клавиатуры — она отъедает низ. */
    void (*layout)(int x, int y, int w, int h);
    void (*render)(void);

    /* type — одно из HAL_EV_POINTER_*. Координаты уже в пикселях экрана. */
    void (*on_touch)(int type, int x, int y);
    void (*on_key)(int key);

    /* Нужна ли этому приложению экранная клавиатура. */
    int  (*wants_keyboard)(void);
} touch_app_t;

extern const touch_app_t app_terminal;
extern const touch_app_t app_settings;
extern const touch_app_t app_files;
extern const touch_app_t app_about;

/* --- Настройки, общие для оболочки и приложения "Настройки" --- */
int  touch_ui_text_scale(void);
void touch_ui_set_text_scale(int scale);

#define WALLPAPER_GRADIENT 0
#define WALLPAPER_SOLID    1
int  touch_ui_wallpaper_mode(void);
void touch_ui_set_wallpaper_mode(int mode);

int  touch_ui_touch_dots(void);
void touch_ui_set_touch_dots(int enabled);

/* Перейти на домашний экран — приложениям иногда нужно закрыть себя. */
void touch_ui_go_home(void);

/* Открыть другое приложение по имени. Нужно для перехода
 * "Настройки -> О системе", как на настоящем телефоне. Имя сравнивается с
 * полем name из touch_app_t; неизвестное имя просто игнорируется. */
void touch_ui_open(const char *app_name);

#endif
