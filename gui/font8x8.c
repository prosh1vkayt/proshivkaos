/* gui/font8x8.c — минимальный блочный 8x8-шрифт (сгенерирован и визуально
 * проверен скриптом gen_font.py). Покрывает A-Z, 0-9 и всю печатную
 * пунктуацию ASCII. Строчные буквы по-прежнему приводятся к заглавным —
 * это осознанная черта оформления системы, а не пробел в шрифте.
 */
#include <stdint.h>

/* Возвращает указатель на 8 байт битмапа символа c (заглавные буквы,
 * цифры, базовая пунктуация). Неизвестные символы -> пустой глиф. */
const uint8_t *font8x8_get_glyph(char c) {
    static const uint8_t empty[8] = {0,0,0,0,0,0,0,0};
    static const uint8_t g_32[8] = {0,0,0,0,0,0,0,0};
    static const uint8_t g_33[8] = {32,32,32,32,32,0,32,0};
    static const uint8_t g_39[8] = {32,32,0,0,0,0,0,0};
    static const uint8_t g_40[8] = {16,32,64,64,64,32,16,0};
    static const uint8_t g_41[8] = {64,32,16,16,16,32,64,0};
    static const uint8_t g_44[8] = {0,0,0,0,0,48,48,96};
    static const uint8_t g_45[8] = {0,0,0,252,0,0,0,0};
    static const uint8_t g_46[8] = {0,0,0,0,0,48,48,0};
    static const uint8_t g_47[8] = {4,8,16,32,64,128,0,0};
    static const uint8_t g_48[8] = {120,140,148,164,196,132,120,0};
    static const uint8_t g_49[8] = {32,96,32,32,32,32,112,0};
    static const uint8_t g_50[8] = {120,132,8,16,32,64,252,0};
    static const uint8_t g_51[8] = {120,132,8,48,8,132,120,0};
    static const uint8_t g_52[8] = {24,40,72,136,252,8,8,0};
    static const uint8_t g_53[8] = {252,128,248,4,4,132,120,0};
    static const uint8_t g_54[8] = {120,128,128,248,132,132,120,0};
    static const uint8_t g_55[8] = {252,4,8,16,32,64,64,0};
    static const uint8_t g_56[8] = {120,132,132,120,132,132,120,0};
    static const uint8_t g_57[8] = {120,132,132,124,4,4,120,0};
    static const uint8_t g_58[8] = {0,48,48,0,48,48,0,0};
    static const uint8_t g_62[8] = {128,64,32,16,32,64,128,0};
    static const uint8_t g_63[8] = {120,132,8,16,32,0,32,0};
    static const uint8_t g_65[8] = {48,72,132,132,252,132,132,0};
    static const uint8_t g_66[8] = {248,132,132,248,132,132,248,0};
    static const uint8_t g_67[8] = {120,132,128,128,128,132,120,0};
    static const uint8_t g_68[8] = {248,132,132,132,132,132,248,0};
    static const uint8_t g_69[8] = {252,128,128,248,128,128,252,0};
    static const uint8_t g_70[8] = {252,128,128,248,128,128,128,0};
    static const uint8_t g_71[8] = {120,132,128,184,132,132,120,0};
    static const uint8_t g_72[8] = {132,132,132,252,132,132,132,0};
    static const uint8_t g_73[8] = {112,32,32,32,32,32,112,0};
    static const uint8_t g_74[8] = {24,8,8,8,8,136,112,0};
    static const uint8_t g_75[8] = {136,144,160,192,160,144,136,0};
    static const uint8_t g_76[8] = {128,128,128,128,128,128,252,0};
    static const uint8_t g_77[8] = {132,204,180,132,132,132,132,0};
    static const uint8_t g_78[8] = {132,196,164,148,140,132,132,0};
    static const uint8_t g_79[8] = {120,132,132,132,132,132,120,0};
    static const uint8_t g_80[8] = {248,132,132,248,128,128,128,0};
    static const uint8_t g_81[8] = {120,132,132,132,148,136,120,8};
    static const uint8_t g_82[8] = {248,132,132,248,144,136,132,0};
    static const uint8_t g_83[8] = {120,128,128,120,4,4,120,0};
    static const uint8_t g_84[8] = {252,32,32,32,32,32,32,0};
    static const uint8_t g_85[8] = {132,132,132,132,132,132,120,0};
    static const uint8_t g_86[8] = {132,132,132,132,132,72,48,0};
    static const uint8_t g_87[8] = {132,132,132,132,180,204,132,0};
    static const uint8_t g_88[8] = {132,72,48,48,72,132,132,0};
    static const uint8_t g_89[8] = {132,72,48,32,32,32,32,0};
    static const uint8_t g_90[8] = {252,4,8,16,32,64,252,0};
    static const uint8_t g_95[8] = {0,0,0,0,0,0,252,0};

    /* Пунктуация, добавленная под тач-сборку: без неё промпт
       "root@proshivkaOS:~#" рисовался с дырами на месте @, ~ и #,
       а экранная клавиатура не могла напечатать половину символов. */
    static const uint8_t g_34[8] = {72,72,0,0,0,0,0,0};
    static const uint8_t g_35[8] = {72,252,72,72,252,72,0,0};
    static const uint8_t g_36[8] = {32,120,160,112,40,240,32,0};
    static const uint8_t g_37[8] = {196,200,16,32,64,152,24,0};
    static const uint8_t g_38[8] = {96,144,160,64,168,144,104,0};
    static const uint8_t g_42[8] = {0,72,48,252,48,72,0,0};
    static const uint8_t g_43[8] = {0,32,32,248,32,32,0,0};
    static const uint8_t g_59[8] = {0,48,48,0,48,48,96,0};
    static const uint8_t g_60[8] = {8,16,32,64,32,16,8,0};
    static const uint8_t g_61[8] = {0,0,252,0,252,0,0,0};
    static const uint8_t g_64[8] = {120,132,180,180,184,128,120,0};
    static const uint8_t g_91[8] = {112,64,64,64,64,64,112,0};
    static const uint8_t g_92[8] = {128,64,32,16,8,4,0,0};
    static const uint8_t g_93[8] = {112,16,16,16,16,16,112,0};
    static const uint8_t g_94[8] = {48,72,132,0,0,0,0,0};
    static const uint8_t g_96[8] = {64,32,0,0,0,0,0,0};
    static const uint8_t g_123[8] = {24,32,32,192,32,32,24,0};
    static const uint8_t g_124[8] = {32,32,32,32,32,32,32,0};
    static const uint8_t g_125[8] = {192,32,32,24,32,32,192,0};
    static const uint8_t g_126[8] = {0,0,100,152,0,0,0,0};

    switch (c) {
        case ' ': return g_32;
        case '!': return g_33;
        case '\'': return g_39;
        case '(': return g_40;
        case ')': return g_41;
        case ',': return g_44;
        case '-': return g_45;
        case '.': return g_46;
        case '/': return g_47;
        case '0': return g_48;
        case '1': return g_49;
        case '2': return g_50;
        case '3': return g_51;
        case '4': return g_52;
        case '5': return g_53;
        case '6': return g_54;
        case '7': return g_55;
        case '8': return g_56;
        case '9': return g_57;
        case ':': return g_58;
        case '>': return g_62;
        case '?': return g_63;
        case 'A': return g_65;
        case 'B': return g_66;
        case 'C': return g_67;
        case 'D': return g_68;
        case 'E': return g_69;
        case 'F': return g_70;
        case 'G': return g_71;
        case 'H': return g_72;
        case 'I': return g_73;
        case 'J': return g_74;
        case 'K': return g_75;
        case 'L': return g_76;
        case 'M': return g_77;
        case 'N': return g_78;
        case 'O': return g_79;
        case 'P': return g_80;
        case 'Q': return g_81;
        case 'R': return g_82;
        case 'S': return g_83;
        case 'T': return g_84;
        case 'U': return g_85;
        case 'V': return g_86;
        case 'W': return g_87;
        case 'X': return g_88;
        case 'Y': return g_89;
        case 'Z': return g_90;
        case '_': return g_95;
        case '"': return g_34;
        case '#': return g_35;
        case '$': return g_36;
        case '%': return g_37;
        case '&': return g_38;
        case '*': return g_42;
        case '+': return g_43;
        case ';': return g_59;
        case '<': return g_60;
        case '=': return g_61;
        case '@': return g_64;
        case '[': return g_91;
        case '\\': return g_92;
        case ']': return g_93;
        case '^': return g_94;
        case '`': return g_96;
        case '{': return g_123;
        case '|': return g_124;
        case '}': return g_125;
        case '~': return g_126;
        default: return empty;
    }
}
