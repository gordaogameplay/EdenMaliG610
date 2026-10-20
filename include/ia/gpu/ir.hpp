// EdenMaliIA — o IR dos shaders, para o subconjunto mínimo.
//
// O que este IR é: a forma intermédia entre SASS Maxwell e GLSL. É pequeno de
// propósito — cobre só as operações de que um fragment shader sem texturas
// precisa. Não é o IR do Eden, e não finge ser.
//
// O que este IR NÃO é: um IR completo. Faltam texturas, controlo de fluxo,
// predicados, subgrupos, e mais uma dúzia de coisas. Cada uma delas é um
// bloco a seguir, e cada uma precisa de ser testada com o bloco anterior a
// funcionar. Um IR que finge cobrir tudo e não cobre nada não serve para
// nada — é o que a telemetria deste projecto aprendeu a dizer.
//
// A regra que governa isto: cada operação aqui existe porque uma medição
// mostrou que o guest a emite. Não se escreve um operando "porque faz falta
// para um shader bonito".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ia::gpu::ir {

// Tipos que um registo Maxwell pode ter.
//
// Maxwell não é tipado: R0..R15 são 16 registos de 32 bits e o TIPO vem da
// instrução que os escreve. `FADD` escreve float em R0; `IADD3` escreve int no
// mesmo R0. O IR reflecte isso: um registo é um índice, e o VALOR tem tipo.
enum class Type : std::uint8_t { U32, F32, U64, F64, Void };

std::string_view type_name(Type t) noexcept;

// Um valor: onde vem.
struct Value {
    enum class Kind : std::uint8_t {
        Reg,     // um registo R0..R15
        Imm32,   // um imediato de 20 ou 32 bits
        Imm64,
        Cbuf,    // c[binding][offset] — a constante
    };

    Kind        kind = Kind::Reg;
    Type        type = Type::U32;
    std::uint8_t reg = 0;      // para Kind::Reg
    std::uint64_t imm = 0;     // para Imm32/Imm64
    std::uint32_t binding = 0; // para Cbuf
    std::uint32_t offset = 0;  // em BYTES, como o IR do Eden — a conversão
                               // de palavra para byte é um bug que se paga
                               // uma vez só
};

// As operações. Cada uma tem de existir porque o guest a emite; o subconjunto
// é o que um fragment shader sem texturas consegue usar.
enum class Op : std::uint8_t {
    Mov,       // Rd = a
    FAdd,      // Rd = a + b        (float)
    FMul,      // Rd = a * b        (float)
    IAdd3,     // Rd = a + b        (inteiro, 32 bits)
    IAdd,      // Rd = a + b        (inteiro)
    ISCAdd,    // Rd = (a << shift) + b     (o opcodes mais comum da Switch)
    LOp3,      // Rd = a & b ^ c    (logica)
    Exit,      // fim do programa
    StoreColor,// escreve o alvo (o que o guest faz com ST.E.CON)
    Unknown,   // opcode fora do subconjunto
};

std::string_view op_name(Op op) noexcept;

// Uma instrução do IR.
struct Instr {
    Op    op = Op::Unknown;
    Type  type = Type::Void;   // o tipo do resultado
    std::uint8_t dst = 0;      // registo destino, quando há
    Value a, b, c;
    bool  is_c = false;        // `c` foi preenchido

    // Campos do ISCADD. Vêm aqui e não dentro de `a`/`b` porque são
    // modificadores da operação, não operandos.
    //
    // `shift` são os 5 bits de [39, 44). `neg_a`/`neg_b` são os bits 49/48.
    // E `condicionado` é o caso em que os dois estão ligados — que não são
    // duas negações, é a forma PREDICADA do opcode. O IR não tem predicados;
    // o que se faz é o mesmo que o Eden faz, que é somar 1 a `b`. Está
    // escrito porque essa aproximação não é uma predicção e um shader que a
    // use a sério vai dar um resultado errado sem nenhum aviso.
    std::uint8_t shift = 0;
    bool          neg_a = false;
    bool          neg_b = false;
    bool          condicionado = false;

    // Nome da opcode Maxwell que originou isto, só para diagnóstico.
    // Um IR que não sabe de onde veio é um IR em que um erro de descodificação
    // é indistinguível de um erro de tradução.
    std::string from_opcode;
};

// Um programa no IR.
struct Program {
    std::vector<Instr> code;
    std::string        stage;     // "fragment" | "vertex" | "compute"
    std::string        from_opcode;  // opcode da SASS que deu origem, se houver

    // Quantas instruções caíram fora do subconjunto. Zero é o objectivo.
    // Um número grande é um relatório honesto sobre o quanto falta, e vale
    // mais do que um tradutor que parece funcionar.
    std::size_t unsupported = 0;
};

} // namespace ia::gpu::ir