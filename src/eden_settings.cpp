// A definição do `Settings::values` do Eden.
//
// Ver `vendor/eden/common/settings_bridge.h` para o que substitui e para a
// medida que decidiu o conteúdo.
#include "vendor/eden/common/settings_bridge.h"

namespace Settings {

// Uma função-local estática, com inicialização na primeira chamada. É o
// motivo de o Eden declarar `values` como uma função e não como uma variável:
// duas translation units que inicializam globais não têm ordem garantida
// entre elas, e uma delas pode ler um `values` ainda a meio de construir.
//
// Aqui a concern é a mesma e a solução também. E a diferença em relação ao
// Eden é que este objectivo não é o nosso: o nosso é que o ficheiro esteja no
// repositório com os dois campos que alguém vai ler, e não o padrão de
// inicialização que o Eden usa.
Values& ValuesRef() {
    static Values v;
    return v;
}

// A referência global. Ver a nota no cabeçalho.
Values& values = ValuesRef();

} // namespace Settings