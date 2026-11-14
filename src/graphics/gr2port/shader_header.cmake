# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

# Writes HEADER_FILE: the words of the SPIR-V module SPIRV_FILE as the array SYMBOL.
file(READ "${SPIRV_FILE}" MODULE HEX)
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," WORDS "${MODULE}")
string(REGEX REPLACE "(0x........,0x........,0x........,0x........,0x........,0x........,)"
       "    \\1\n" WORDS "${WORDS}")
file(WRITE "${HEADER_FILE}"
     "#pragma once\n#include <cstdint>\nconst uint32_t ${SYMBOL}[] = {\n${WORDS}\n};\n")
