// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — tradução Maxwell -> Valhall.
//
// A GPU do Switch é uma NVIDIA Maxwell (GM20B). A do Orange Pi 5 Pro é uma
// ARM Mali-G610, que implementa Valhall. Não é possível executar os comandos
// Maxwell: o formato do push buffer é proprietário da NVIDIA.
//
// A tradução não é emulation. O driver da NVIDIA no Switch é um programa
// cliente que escreve comandos para o push buffer; esse buffer é um formato de
// comandos intermédio. O que este ficheiro faz é ler esse formato e converter
// cada comando no equivalente Vulkan, que o PanVK compila para Valhall.
//
// Equivalências estruturais a respeitar:
//
//   Maxwell (NVIDIA)              ->  Valhall (Mali/Vulkan)
//   ------------------------------    ------------------------------------
//   shaders GLSL/GLASM            ->  SPIR-V
//   NV_GEOMETRY_SHADER             ->  sem equivalente; degrada para VS+FS
//   tessellation                   ->  sem equivalente; degrada
//   clip/cull distance             ->  ignorado (clipDistances = 0)
//   multi-viewport                 ->  1 viewport
//   Tessellation/Geometry passes   ->  eliminados
//   render target D32_FLOAT        ->  D24/D32 não escalável na Mali;
//                                       fallback obrigatório
//
// A Mali-G610 é tiler de alto fill-rate e baixa largura de banda. O
// desempenho depende sobretudo de:
//   - quantos tiles e quantas passagens de carga/store (evitar overdraw),
//   - o formato dos alvos de render (formatos estreitos são mais rápidos),
//   - o tamanho dos batches de submissão.
//
// A telemetria mede o custo de traduzir cada comando, para que se possa ver
// se a tradução (CPU) ou a execução (GPU) é que trava o frame.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "g610/telemetry.hpp"

namespace g610::gpu {

// ---------------------------------------------------------------------------
// Comandos Maxwell.
//
// ENCODING (Maxwell push buffer)
// --------------------------------
// A primeira palavra de cada comando é o cabeçalho:
//
//     bit 0        bit 1        bits 2..15
//   payload?    has more?      opcode
//
//   payload    — se 1, a palavra seguinte é o comprimento do payload, em
//                bytes, e os parâmetros vêm a seguir.
//   has more   — se 1, existe um segundo bloco de payload.
//
// Os valores abaixo são o campo `opcode` já deslocado (isto é, head >> 2).
//
// NOTA HONESTA — este descodificador é um esqueleto
// -----------------------------------------------------
// A numeração real de opcodes da NVIDIA não é pública e o que está na
// tabela pode não coincidir com o que um dump real de push buffer contém.
// O que É correcto e é testado aqui é a parte estrutural, que é a que
// precisa de ser certa antes de a GPU funcionar:
//
//   - o bit de payload é interpretado à vontade,
//   - o comprimento do payload é respeitado e nunca lido fora do buffer,
//   - um opcode desconhecido é saltado com segurança, contando-o,
//   - a exaustão do buffer é detectada em vez de correr fora da memória.
//
// Antes de isto correr num jogo real é preciso validar a tabela contra um
// dump capturado. Ver docs/ARQUITETURA.md, fase 3.
// ---------------------------------------------------------------------------
// Só entram aqui opcodes com valor distinto. Vários opcodes Maxwell partilham
// o mesmo campo de 2 bits e distinguem-se por um sub-opcode dentro do payload;
// esses não são enumerados — são tratados como "outro" e contados. Um enum com
// valores duplicados produziria etiquetas de `case` duplicadas no switch, que
// não compilam, e forçaria a distinguir ops que o descodificador ainda não sabe
// distinguir.
enum class MaxwellOp : std::uint32_t {
    Nop                    = 0x000,
    SetStencil             = 0x004,
    SetViewport            = 0x00B,
    Clear                  = 0x013,
    SetState               = 0x012,
    BindShader             = 0x016,
    DrawArrays             = 0x018, // 0x62 >> 2
    DrawElementsBaseVertex = 0x01A, // 0x68 >> 2
    DrawInstanced          = 0x020, // 0x82/0x84 >> 2
    SetScissor             = 0x029,
    SetDepthRange          = 0x03D,
    SemaphoreIncrement     = 0x044,
    BindComputeProgram     = 0x06C,
    DispatchCompute        = 0x06E,
};

std::string to_string(MaxwellOp op);

// ---------------------------------------------------------------------------
// Um comando já traduzido.
// ---------------------------------------------------------------------------
struct DrawCommand {
    MaxwellOp     op{};
    std::uint32_t instance_count = 0;
    std::uint32_t vertex_count   = 0;
    std::uint32_t index_count    = 0;
    std::uint32_t base_vertex    = 0;
    std::uint32_t group_x = 1, group_y = 1, group_z = 1;

    // Onde vivem os shaders, já convertidos para SPIR-V.
    std::uint32_t vertex_shader   = 0;
    std::uint32_t fragment_shader = 0;

    // Preenchimento estimado, para decidir onde vale a pena lembrete.
    std::uint64_t est_triangles = 0;
};

// ---------------------------------------------------------------------------
// Tradutor de shaders.
// ---------------------------------------------------------------------------
// A recompilação de shaders Maxwell -> SPIR-V é o custo dominante do arranque:
// da primeira vez que um shader é visto, compila-se; depois vem de cache.
// A telemetria separa "shader novo" de "cache hit" porque essa é a diferença
// entre um jogo que arranca rápido e um que engasga a cada nível.
class ShaderCache {
public:
    explicit ShaderCache(tel::Telemetry& tel) : tel_(tel) {}

    // Converte um shader Maxwell para SPIR-V.
    // Devolve o índice do shader no vector interno, ou UINT32_MAX se a
    // conversão falhar. `hit` diz se veio de cache.
    std::uint32_t compile(const std::vector<std::uint8_t>& maxwell_blob,
                          bool is_vertex, bool& hit, std::string& error);

    std::size_t size() const { return shaders_.size(); }
    void clear();

    // Taxa de acerto do cache, 0..1.
    double hit_rate() const;

private:
    struct Entry {
        std::vector<std::uint32_t> spirv;
        bool        vertex  = false;
        std::size_t source_bytes = 0;
    };
    tel::Telemetry& tel_;
    std::vector<Entry> shaders_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
    std::uint64_t hits_ = 0, misses_ = 0;
};

// ---------------------------------------------------------------------------
// Tradutor Maxwell -> Vulkan.
// ---------------------------------------------------------------------------
class MaxwellTranslator {
public:
    explicit MaxwellTranslator(tel::Telemetry& tel) : tel_(tel), shaders_(tel) {}

    // Lê o push buffer e devolve a lista de comandos já extraídos.
    // Não executa Vulkan; essa é a responsabilidade de ValhallBackend.
    std::size_t translate(const std::vector<std::uint8_t>& pushbuffer,
                          std::vector<DrawCommand>& out, std::string& error);

    ShaderCache& shaders() { return shaders_; }

    // Estatísticas de tradução.
    std::uint64_t commands_total() const { return cmds_total_; }
    std::uint64_t commands_unknown() const { return cmds_unknown_; }
    std::uint64_t commands_dropped_feature() const { return cmds_dropped_; }

    // Traduz o limite do PushBuffer. Se devolve false, o buffer não coube e o
    // erro é reportado — um runaway de writes é um sintoma, não um detalhe.
    bool drain_pushbuffer(std::vector<std::uint8_t>& buffer,
                          std::size_t& consumed, std::string& error);

private:
    tel::Telemetry&  tel_;
    ShaderCache      shaders_;
    std::uint64_t    cmds_total_ = 0;
    std::uint64_t    cmds_unknown_ = 0;
    std::uint64_t    cmds_dropped_ = 0;
};

} // namespace g610::gpu
