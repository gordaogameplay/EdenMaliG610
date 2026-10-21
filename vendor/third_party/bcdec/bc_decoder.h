// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// O `video_core` do upstream inclui <bc_decoder.h> — o nome que o Eden dá
// ao `bcdec` do iOrange. O codigo de origem chama-se `bcdec.h`.
//
// Este e' o shim: o nome que o upstream espera, a libraria que ele
// usa. Nao ha' codigo duplicado — e' o mesmo codigo, com o nome certo.
#pragma once

#include "bcdec.h"
