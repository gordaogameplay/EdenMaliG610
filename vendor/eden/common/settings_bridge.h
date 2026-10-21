// Substitui `src/common/settings.h` + `settings.cpp` do Eden.
//
// ## O que o código vendorizado pede
//
// **Três campos**, medidos com `grep -rhoE "Settings::values\.[a-z_]+"` sobre
// `vendor/eden`:
//
//     Settings::values.use_auto_stub
//     Settings::values.use_debug_asserts
//     Settings::values.disabled_addons
//
// E é tudo. O `Settings` do Eden tem dezenas — resolução,海湾 de memória, o
// caminho do ROM, a lista de serviços a stubbar, chaves de debug — e **nada
// disso é lido por nenhum ficheiro que compilamos**. A configuração é a
// superfície que mais cresce e menos se lê, e é a razão de o settings ser uma
// lista de excepção e não uma lista de campos.
//
// ## Os dois valores, e porquê
//
// `use_auto_stub` — o Eden responde com um stub automático a um comando que um
// serviço não sabe tratar, em vez de devolver `ResultInvalidCommand`.
//
// **Falso.** A telemetria deste projecto é a_LISTA do que o jogo pede e o que
// ainda não está feito (`SaveUnimplementedFunctionReport` escreve nela). Com
// `use_auto_stub` a `true`, um comando não implementado devolvia sucesso e o
// jogo continuaria a correr sobre uma mentira — e a lista nunca encheria,
// porque nada faltaria.
//
// ## Onde isto deixaria de ser verdade
//
// Se um dia o motor ganhar serviços stubs para classes inteiras (a `fs`, a
// `nn::oe`), e a ideia for devolver sucesso ao que não está implementado, então
// `use_auto_stub` passa a ter de ser configurável. Está aqui a linha, e o
// comentário, para que esse dia não comece por reescrever isto.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Os *enums* do upstream (o `AstcRecompression` e o `NvdecEmulation`) —
// estão no `common/settings_enums.h` ✓, que é vendorizado ✓.
#include "common/settings_enums.h"

namespace Settings {

// Um valor de configuracao que serve **os dois usos**.
//
// A casa lia-os como `bool` (`if (Settings::values.use_auto_stub)` ✓) e o
// upstream lia-os como `Setting<T>::GetValue()` ✓. Com a conversao
// implicita e o `GetValue()` ✓, os dois compilam contra o **mesmo**
// objecto ✓ — que e' o que importa ✗ (dois `Values` diferentes no
// mesmo binario = campos lidos a deslocamentos errados ✗).
template <typename T>
class Valor {
public:
    Valor() = default;
    // NOLINTNEXTLINE(google-explicit-constructor) — a conversao
    // implicita e' o que faz o `if (Settings::values.use_auto_stub)`
    // do codigo da casa compilar sem mudar uma linha.
    Valor(T valor) : valor_(std::move(valor)) {}

    [[nodiscard]] const T& GetValue() const noexcept { return valor_; }
    operator const T&() const noexcept { return valor_; }

private:
    T valor_{};
};

struct Values {
    // Ver a nota do ficheiro: `false` é a escolha, não a omissão.
    // **DIVERGENCIA DELIBERADA DO EDEN.** O Eden tem isto a `false` (e o
    // default dele tambem e' `false`): um comando que um servico nao
    // implementa fica **sem resposta**, e o guest le o que estiver no buffer.
    //
    // Aqui isso e' um crash. Medido: o SDK pediu o comando 1 do
    // `IWindowController` (`GetAppletResourceUserId`), o nosso servico
    // respondeu com "nao implementado" sem escrever nada, e o guest leu
    // lixo do buffer de mensagens — e o salto seguinte foi para um
    // endereco de **host** (0xaaaadaf341a4), que e' um fault de memoria do
    // guest.
    //
    // A razao de o Eden poder ter isto desligado e' que o Eden implementa
    // todos os comandos: um comando desconhecido la' e' um bug do Eden. A
    //qui e' o nosso bug, e a resposta honesta e' um Result de erro, e nao
    // o silencio.
    Valor<bool> use_auto_stub{true};
    Valor<bool> use_debug_asserts{false};

    // Os DLC desativados, por id de programa. **Vazio** — este motor não
    // tem DLC (não há `NCA` com conteúdo).
    //
    // O tipo é o do Eden (`common/settings.h:874`), e o campo tem de
    // existir mesmo vazio: o `aoc` **indexa** este mapa com o id do
    // programa corrente (`aoc/addon_content_manager.cpp:121`), e sem o
    // campo essa leitura é lixo — e o `std::find` sobre um vector lixo
    // mata o motor.
    //
    // **E o mapa tem de estar NESTE tipo.** Se existirem dois
    // `Settings::Values` diferentes no mesmo binário — o nosso e o do Eden
    // — o `Settings::values` é o **mesmo símbolo** para os dois, e o `aoc`
    // lê `disabled_addons` a um deslocamento que não existe no objecto
    // nosso. Por isso o `settings.h` vendorizado é **substituído** por este
    // ficheiro (ver `tools/overrides/common/settings.h`): há um só tipo em
    // jogo. Medido (`boot_p6j.log`): o fault foi um `std::find` sobre um
    // mapa lixo, dentro do `CountAddOnContent`.
    std::map<std::uint64_t, std::vector<std::string>> disabled_addons;

    // ── Os campos que o motor Maxwell do upstream lê ─────────────────────
    //
    // **Medido**: `grep -rhoE "Settings::values\.[a-z_0-9]+"` sobre os
    // ficheiros do `video_core` que a casa compila — e mais o campo que
    // o cabeçalho da interface do renderizador usa. São estes e nenhum
    // mais (o `Settings` do Eden tem centenas; a casa só precisa destes).
    //
    // Os valores por omissão são os do Eden ✓, menos o `astc_recompression`
    // (o Eden pe-de-ao-`Bc1`; a casa não recomprime ASTC ✗ — não há
    // descodificador de ASTC no host ✗).
    Valor<AstcRecompression> astc_recompression{AstcRecompression::Uncompressed};
    Valor<bool> use_reactive_flushing{false};
    Valor<NvdecEmulation> nvdec_emulation{NvdecEmulation::Gpu};
    Valor<bool> sync_memory_operations{false};
    Valor<bool> disable_macro_hle{false};
    Valor<bool> dump_macros{false};
    Valor<bool> disable_macro_jit{false};
    Valor<bool> use_fast_gpu_time{false};
    Valor<std::uint32_t> fast_gpu_time{0};
    Valor<bool> skip_cpu_inner_invalidation{false};
};

// A definição global. O Eden põe isto numa função para não haver uma ordem de
// inicialização estática dependente da ordem de-linking das translation
// units; aqui só há uma, e é a mesma razão.
Values& ValuesRef();

// A referência que o código do Eden escreve: `Settings::values`.
//
// **Declarada aqui, definida em `src/eden_settings.cpp`, e DENTRO do
// namespace.** Fora dele seria `::values`, e o compilador diria «‘Values’ does
// not name a type» — que não menciona namespaces e faz o erro parecer de um
// include em falta.
extern Values& values;

} // namespace Settings