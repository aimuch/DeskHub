#!/usr/bin/env python3
"""Break the ASCII editor on a temporary copy; the prewritten input cases must fail."""
from _engine import firmware_main
H = "components/ui/include/ui_keyboard.hpp"
M = [
    ("controls accepted", H, "c < 32 || c > 126", "c < 0 || c > 126"),
    ("password length cap missing", H, "length_ >= (password_ ? 64u : 32u)", "length_ >= (password_ ? 65u : 32u)"),
    ("name length cap missing", H, "length_ >= (password_ ? 64u : 32u)", "length_ >= (password_ ? 64u : 33u)"),
    ("64 byte nonhex accepted", H, "length_ == 63 && (!hex()", "length_ == 64 && (!hex()"),
    ("password never masked", H, "password_ && !revealed_ ?", "password_ && false ?"),
    ("reveal never shows", H, "revealed_ = password_ && on;", "revealed_ = password_ && on && false;"),
    ("erase does nothing", H, "if (length_) bytes_[--length_] = 0;", "if (length_) bytes_[length_] = 0;"),
]
if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "keyboard_test"))
