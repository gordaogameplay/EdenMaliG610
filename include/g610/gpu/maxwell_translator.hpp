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

#include "g610/gpu/maxwell_comandos.hpp"
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
// Os comandos do Maxwell 3D.
//
// **Estes IDs não são adivinhados**: são os índices dos registos no
// motor do upstream (`offsetof(Regs, campo) / 4` — a macro
// `MAXWELL3D_REG_INDEX` ✓), extraídos *compilando* o cabeçalho do
// upstream ✓ (`tools/extrair_comandos_maxwell.py` ✓). Ver
// `g610/gpu/maxwell_comandos.hpp` ✓ — 168 comandos ✓.
//
// O ID de um método **é** o índice do registo ✓: no push buffer, o
// cabeçalho do método traz esse ID nos bits 0..12 ✓ (ver
// `g610/gpu/pushbuffer.hpp` ✓).
// ---------------------------------------------------------------------------
// Atributo de vértice decodificado (Maxwell `vertex_attrib_format`).
// ---------------------------------------------------------------------------
struct VertexAttributeDesc {
    bool enabled = false;
    std::uint32_t buffer = 0;        // stream index
    std::uint32_t offset = 0;        // byte offset no vértice
    std::uint32_t size_enum = 0;     // Size
    std::uint32_t type_enum = 0;     // Type
    bool constant = false;
    bool bgra = false;
    std::uint32_t component_count = 0;
    std::uint32_t size_bytes = 0;
    std::uint32_t stream_stride = 0;
};

// ---------------------------------------------------------------------------
// Um comando já traduzido.
// ---------------------------------------------------------------------------
struct DrawCommand {
    // O método que deu origem ao desenho (o ID real ✓).
    std::uint32_t method = 0;
    // Se o desenho é indexado (o upstream: `draw_state.draw_indexed` ✓).
    bool           indexado = false;
    std::uint32_t instance_count = 0;
    std::uint32_t vertex_count   = 0;
    std::uint32_t index_count    = 0;
    std::uint32_t index_first    = 0;
    std::uint32_t base_vertex    = 0;
    std::uint32_t group_x = 1, group_y = 1, group_z = 1;

    // Dados do stream de vértices (Maxwell `vertex_streams[0]`), capturados
    // do registo 0x1C00..0x1C02 quando presentes. 0 = não capturado.
    // **Ainda não são lidos da memória do guest** — ver ETAPA 4.
    std::uint64_t vertex_base_va = 0;
    std::uint32_t vertex_stride  = 0;

    // Atributos decodificados (apenas os habilitados).
    std::vector<VertexAttributeDesc> attributes;

    // Topologia: 0 = não determinada.
    std::uint32_t topology_raw = 0;
    std::uint32_t primitive_topology_control_raw = 0;

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

    // O acumulado do microcode: os passos e os envios de todas as
    // macros executadas (`mme::Interpretador::passos`/`envios` — cada
    // `executar` reinicia a sua contagem; o tradutor soma os totais).
    std::uint64_t macro_passos() const { return macro_passos_; }
    std::uint64_t macro_envios() const { return macro_envios_; }

    // O histograma dos envios das macros na ULTIMA `translate`: metodo ->
    // quantas vezes o `Send` as entregou. Instrumento do passo 1: o
    // "0 comandos traduzidos" com 901 envios so' se decide com os ids na
    // mao (se um deles for o draw_end/1413 e' um bug da maquina; se for
    // setup, o jogo nao comecou a desenhar). Zerado no comeco de cada
    // `translate`, como o `falcon_vas()`.
    const std::unordered_map<std::uint32_t, std::uint32_t>& envio_ids() const {
        return envio_ids_;
    }

    // Os endereços que as macros entregaram ao `falcon[4]` na ÚLTIMA
    // `translate` (zerado no início dela). É a medição do que o blob
    // da chamada de firmware 4 recebe: no run vivo, o nvdrv lê a
    // memória do guest nesses VAs (o instrumento `G610_VA_DUMP`) —
    // o verificador do conteúdo antes de qualquer decisão de desenho.
    const std::vector<std::uint32_t>& falcon_vas() const { return falcon_vas_; }

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
    // O acumulado do microcode (o mesmo produtor de sintomas do
    // `mme::Interpretador`; aquí somado entre execuções).
    std::uint64_t    macro_passos_ = 0;
    std::uint64_t    macro_envios_ = 0;
    // O histograma por metodo dos `Send` das macros na translate corrente
    // (zerada no começo dela; a vista publica acima).
    std::unordered_map<std::uint32_t, std::uint32_t> envio_ids_;
    // Os VAs entregues ao `falcon[4]` na translate corrente (zerada no
    // começo dela; a ordem do recebimento preservada, sem deduplicar —
    // a contagem por submit também é medida).
    std::vector<std::uint32_t> falcon_vas_;
};

} // namespace g610::gpu
