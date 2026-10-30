// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — Shaders SPIR-V embutidos (vertex + fragment).
//
// Compilado com glslangValidator a partir de GLSL simples:
//   vertex.vert:  posicao (vec2) -> gl_Position
//   fragment.frag: saida de cor vermelha (1,0,0,1)
#pragma once

#include <cstdint>

extern const unsigned char vertex_spv[];
extern const unsigned int vertex_spv_len;
extern const unsigned char fragment_spv[];
extern const unsigned int fragment_spv_len;
