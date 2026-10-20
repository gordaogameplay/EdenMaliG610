// Onde o guest corre. Ver `src/guest_placement.cpp` para o porque.
#pragma once

#include <cstddef>

namespace g610 {

// Põe a thread do guest nos nucleos grandes. Devolve quantos fixou, 0 se
// nao houver big.LITTLE ou se a fixacao falhar.
std::size_t fixar_guest_nos_nucleos_grandes();

} // namespace g610
