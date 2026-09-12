/* apps/editor.h — полноэкранный текстовый редактор поверх hal_console.
 * Бинды в стиле micro: Ctrl+S сохранить, Ctrl+Q выйти, Ctrl+F найти.
 */
#ifndef PROSHIVKAOS_EDITOR_H
#define PROSHIVKAOS_EDITOR_H

/* Открывает файл на весь экран и не возвращает управление, пока
 * пользователь не выйдет (Ctrl+Q). Файла может не существовать — будет
 * создан пустым при первом сохранении, как и в старом cmd_edit. */
void editor_run(const char *path);

#endif
