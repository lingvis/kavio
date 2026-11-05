#pragma once

/** USB HID keyboard usage codes and input operation types. */

#include <cstddef>
#include <cstdint>

namespace kavio {

/**
 * USB HID Keyboard/Keypad usage codes (Usage Page 0x07).
 *
 * Keyboard.h is the single source of named keyboard key codes in the public SDK.
 * The IPC protobuf transports the numeric usage ID and does not duplicate this enum.
 */
#define KAVIO_KEY_CODE_TABLE(X) \
    X(kReserved, RESERVED, 0x00) \
    X(kErrorRollover, ERROR_ROLLOVER, 0x01) \
    X(kErrorPostFail, ERROR_POST_FAIL, 0x02) \
    X(kErrorUndefined, ERROR_UNDEFINED, 0x03) \
    X(kA, A, 0x04) \
    X(kB, B, 0x05) \
    X(kC, C, 0x06) \
    X(kD, D, 0x07) \
    X(kE, E, 0x08) \
    X(kF, F, 0x09) \
    X(kG, G, 0x0A) \
    X(kH, H, 0x0B) \
    X(kI, I, 0x0C) \
    X(kJ, J, 0x0D) \
    X(kK, K, 0x0E) \
    X(kL, L, 0x0F) \
    X(kM, M, 0x10) \
    X(kN, N, 0x11) \
    X(kO, O, 0x12) \
    X(kP, P, 0x13) \
    X(kQ, Q, 0x14) \
    X(kR, R, 0x15) \
    X(kS, S, 0x16) \
    X(kT, T, 0x17) \
    X(kU, U, 0x18) \
    X(kV, V, 0x19) \
    X(kW, W, 0x1A) \
    X(kX, X, 0x1B) \
    X(kY, Y, 0x1C) \
    X(kZ, Z, 0x1D) \
    X(kDigit1, DIGIT_1, 0x1E) \
    X(kDigit2, DIGIT_2, 0x1F) \
    X(kDigit3, DIGIT_3, 0x20) \
    X(kDigit4, DIGIT_4, 0x21) \
    X(kDigit5, DIGIT_5, 0x22) \
    X(kDigit6, DIGIT_6, 0x23) \
    X(kDigit7, DIGIT_7, 0x24) \
    X(kDigit8, DIGIT_8, 0x25) \
    X(kDigit9, DIGIT_9, 0x26) \
    X(kDigit0, DIGIT_0, 0x27) \
    X(kEnter, ENTER, 0x28) \
    X(kEscape, ESCAPE, 0x29) \
    X(kBackspace, BACKSPACE, 0x2A) \
    X(kTab, TAB, 0x2B) \
    X(kSpace, SPACE, 0x2C) \
    X(kMinus, MINUS, 0x2D) \
    X(kEqual, EQUAL, 0x2E) \
    X(kLeftBracket, LEFT_BRACKET, 0x2F) \
    X(kRightBracket, RIGHT_BRACKET, 0x30) \
    X(kBackslash, BACKSLASH, 0x31) \
    X(kNonUsHash, NON_US_HASH, 0x32) \
    X(kSemicolon, SEMICOLON, 0x33) \
    X(kQuote, QUOTE, 0x34) \
    X(kGrave, GRAVE, 0x35) \
    X(kComma, COMMA, 0x36) \
    X(kPeriod, PERIOD, 0x37) \
    X(kSlash, SLASH, 0x38) \
    X(kCapsLock, CAPS_LOCK, 0x39) \
    X(kF1, F1, 0x3A) \
    X(kF2, F2, 0x3B) \
    X(kF3, F3, 0x3C) \
    X(kF4, F4, 0x3D) \
    X(kF5, F5, 0x3E) \
    X(kF6, F6, 0x3F) \
    X(kF7, F7, 0x40) \
    X(kF8, F8, 0x41) \
    X(kF9, F9, 0x42) \
    X(kF10, F10, 0x43) \
    X(kF11, F11, 0x44) \
    X(kF12, F12, 0x45) \
    X(kPrintScreen, PRINT_SCREEN, 0x46) \
    X(kScrollLock, SCROLL_LOCK, 0x47) \
    X(kPause, PAUSE, 0x48) \
    X(kInsert, INSERT, 0x49) \
    X(kHome, HOME, 0x4A) \
    X(kPageUp, PAGE_UP, 0x4B) \
    X(kDelete, DELETE, 0x4C) \
    X(kEnd, END, 0x4D) \
    X(kPageDown, PAGE_DOWN, 0x4E) \
    X(kRight, RIGHT, 0x4F) \
    X(kLeft, LEFT, 0x50) \
    X(kDown, DOWN, 0x51) \
    X(kUp, UP, 0x52) \
    X(kNumLock, NUM_LOCK, 0x53) \
    X(kKeypadSlash, KEYPAD_SLASH, 0x54) \
    X(kKeypadAsterisk, KEYPAD_ASTERISK, 0x55) \
    X(kKeypadMinus, KEYPAD_MINUS, 0x56) \
    X(kKeypadPlus, KEYPAD_PLUS, 0x57) \
    X(kKeypadEnter, KEYPAD_ENTER, 0x58) \
    X(kKeypad1, KEYPAD_1, 0x59) \
    X(kKeypad2, KEYPAD_2, 0x5A) \
    X(kKeypad3, KEYPAD_3, 0x5B) \
    X(kKeypad4, KEYPAD_4, 0x5C) \
    X(kKeypad5, KEYPAD_5, 0x5D) \
    X(kKeypad6, KEYPAD_6, 0x5E) \
    X(kKeypad7, KEYPAD_7, 0x5F) \
    X(kKeypad8, KEYPAD_8, 0x60) \
    X(kKeypad9, KEYPAD_9, 0x61) \
    X(kKeypad0, KEYPAD_0, 0x62) \
    X(kKeypadPeriod, KEYPAD_PERIOD, 0x63) \
    X(kNonUsBackslash, NON_US_BACKSLASH, 0x64) \
    X(kApplication, APPLICATION, 0x65) \
    X(kPower, POWER, 0x66) \
    X(kKeypadEqual, KEYPAD_EQUAL, 0x67) \
    X(kF13, F13, 0x68) \
    X(kF14, F14, 0x69) \
    X(kF15, F15, 0x6A) \
    X(kF16, F16, 0x6B) \
    X(kF17, F17, 0x6C) \
    X(kF18, F18, 0x6D) \
    X(kF19, F19, 0x6E) \
    X(kF20, F20, 0x6F) \
    X(kF21, F21, 0x70) \
    X(kF22, F22, 0x71) \
    X(kF23, F23, 0x72) \
    X(kF24, F24, 0x73) \
    X(kExecute, EXECUTE, 0x74) \
    X(kHelp, HELP, 0x75) \
    X(kMenu, MENU, 0x76) \
    X(kSelect, SELECT, 0x77) \
    X(kStop, STOP, 0x78) \
    X(kAgain, AGAIN, 0x79) \
    X(kUndo, UNDO, 0x7A) \
    X(kCut, CUT, 0x7B) \
    X(kCopy, COPY, 0x7C) \
    X(kPaste, PASTE, 0x7D) \
    X(kFind, FIND, 0x7E) \
    X(kMute, MUTE, 0x7F) \
    X(kVolumeUp, VOLUME_UP, 0x80) \
    X(kVolumeDown, VOLUME_DOWN, 0x81) \
    X(kLockingCapsLock, LOCKING_CAPS_LOCK, 0x82) \
    X(kLockingNumLock, LOCKING_NUM_LOCK, 0x83) \
    X(kLockingScrollLock, LOCKING_SCROLL_LOCK, 0x84) \
    X(kKeypadComma, KEYPAD_COMMA, 0x85) \
    X(kKeypadEqualAs400, KEYPAD_EQUAL_AS400, 0x86) \
    X(kInternational1, INTERNATIONAL_1, 0x87) \
    X(kInternational2, INTERNATIONAL_2, 0x88) \
    X(kInternational3, INTERNATIONAL_3, 0x89) \
    X(kInternational4, INTERNATIONAL_4, 0x8A) \
    X(kInternational5, INTERNATIONAL_5, 0x8B) \
    X(kInternational6, INTERNATIONAL_6, 0x8C) \
    X(kInternational7, INTERNATIONAL_7, 0x8D) \
    X(kInternational8, INTERNATIONAL_8, 0x8E) \
    X(kInternational9, INTERNATIONAL_9, 0x8F) \
    X(kLanguage1, LANGUAGE_1, 0x90) \
    X(kLanguage2, LANGUAGE_2, 0x91) \
    X(kLanguage3, LANGUAGE_3, 0x92) \
    X(kLanguage4, LANGUAGE_4, 0x93) \
    X(kLanguage5, LANGUAGE_5, 0x94) \
    X(kLanguage6, LANGUAGE_6, 0x95) \
    X(kLanguage7, LANGUAGE_7, 0x96) \
    X(kLanguage8, LANGUAGE_8, 0x97) \
    X(kLanguage9, LANGUAGE_9, 0x98) \
    X(kLeftCtrl, LEFT_CTRL, 0xE0) \
    X(kLeftShift, LEFT_SHIFT, 0xE1) \
    X(kLeftAlt, LEFT_ALT, 0xE2) \
    X(kLeftGui, LEFT_GUI, 0xE3) \
    X(kRightCtrl, RIGHT_CTRL, 0xE4) \
    X(kRightShift, RIGHT_SHIFT, 0xE5) \
    X(kRightAlt, RIGHT_ALT, 0xE6) \
    X(kRightGui, RIGHT_GUI, 0xE7)

enum class KeyCode : int32_t {
#define KAVIO_KEY_CODE_ENUM(cpp_name, public_name, value) cpp_name = value,
    KAVIO_KEY_CODE_TABLE(KAVIO_KEY_CODE_ENUM)
#undef KAVIO_KEY_CODE_ENUM
};

namespace detail {

struct KeyCodeDescriptor {
    const char* name;
    KeyCode code;
};

inline constexpr KeyCodeDescriptor kKeyCodeDescriptors[] = {
#define KAVIO_KEY_CODE_DESCRIPTOR(cpp_name, public_name, value) {#public_name, KeyCode::cpp_name},
    KAVIO_KEY_CODE_TABLE(KAVIO_KEY_CODE_DESCRIPTOR)
#undef KAVIO_KEY_CODE_DESCRIPTOR
};

inline constexpr size_t kKeyCodeDescriptorCount =
    sizeof(kKeyCodeDescriptors) / sizeof(kKeyCodeDescriptors[0]);

}  // namespace detail

constexpr int32_t KeyCodeValue(KeyCode code) noexcept
{
    return static_cast<int32_t>(code);
}

#undef KAVIO_KEY_CODE_TABLE

}  // namespace kavio
