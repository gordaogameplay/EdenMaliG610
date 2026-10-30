with open('src/guest_program.cpp', 'r') as f:
    content = f.read()

old = '''bool GuestProgram::init_main_thread_tls(std::string& error) {
    // O bloco estático de TLS da thread. Variante 1 do AArch64: o TCB (dois
    // ponteiros) vem primeiro, e os dados (.tdata) e o resto (.tbss) depois.
    //
    // `getTlsStartOffset()` do libnx devolve `max(__tls_align, 16)` e o tp
    // aponta para `dados - esse_offset`. Não sabemos `__tls_align` do SDK da
    // Nintendo, por isso usamos o mínimo garantido (16, o TCB) e tratamos
    // isto como hipótese a refinar pelo próximo fault, não como facto. Está
    // dito aqui porque publishing um número como se fosse medido seria
    // repetir o erro das máscaras de bits.
    constexpr std::uint64_t kTcbSize  = 16;
    const std::uint64_t static_tls = layout::kTlsStaticBase;
    const std::uint64_t tp          = static_tls + kTcbSize;

    struct Thr {
        std::uint32_t magic;
        std::uint32_t handle;
        std::uint64_t thread_ptr;
        std::uint64_t reent;
        std::uint64_t tls_tp;
    };
    static_assert(sizeof(Thr) == 0x20, "a ThreadLocalRegion tem 0x20 bytes no fim");

    Thr t{};
    t.magic      = layout::kThreadVarsMagic;
    t.handle     = layout::kMainThreadHandle;
    t.thread_ptr = layout::kThreadObject;
    t.reent      = layout::kReentBase;
    t.tls_tp     = tp;

    const std::uint64_t tv = layout::kTlsBase + 0x200 - sizeof(Thr);
    if (!mem_.write(tv, &t, sizeof(t), error))
        return false;

    // O handle da thread, em TLS+0x110, é escrito **depois**, por quem
    // conhece o handle verdadeiro: `SvcDispatcher::set_eden_tls`.
    //
    // Ver a nota longa em `SvcDispatcher::set_eden_tls` — o handle tem de ser
    // o da tabela de handles do Eden, e esse só existe depois de o sistema ser
    // montado. Escrever aqui um número diferente seria dar ao loader um handle
    // que não existe na tabela.

    // A região de TLS entregue ao guest tem de estar a zeros à partida. Não
    // é pprint: é o contrato — se houvesse lixo, o SDK ia ler um `magic`
    // errado e recusar a thread em vez de inicializar.
    //
    // O bloco já está dentro da reserva (que é um `mmap` anónimo, logo zeros),
    // por isso não se mapeia nada aqui. Zera-se à mesma, para que isto não
    // dependa de ser anónimo.
    if (!mem_.owns(static_tls) ||
        !mem_.owns(static_tls + layout::kTlsStaticSize - 1)) {
        error = "o bloco de TLS estático (" + hex(static_tls) +
                ") está fora da reserva do guest";
        return false;
    }
    std::vector<std::uint8_t> zeros(layout::kTlsStaticSize, 0);
    if (!mem_.write(static_tls, zeros.data(), zeros.size(), error))
        return false

    // Os objectos que a ThreadLocalRegion aponta. O SDK desreferencia-os sem
    // os verificar, por isso têm de existir.'''

new = '''bool GuestProgram::init_main_thread_tls(std::string& error) {
    // ThreadLocalRegion layout (libnx variant 1):
    //
    //   kTlsBase + 0x000 .. +0x100   message buffer (256 bytes)
    //   kTlsBase + 0x100 .. +0x1E0   reserved / other fields
    //   kTlsBase + 0x1E0 .. +0x200   Thr struct (magic, handle, thread_ptr, reent, tls_tp)
    //
    // TP (TPIDRRO_EL0) points to TCB at end of ThreadLocalRegion:
    //   TP = kTlsBase + 0x200
    //   TCB at kTlsBase + 0x1F0 .. +0x200 (DTV pointer + self pointer)
    //
    // The Thr struct's tls_tp field stores the TP value.
    //
    // The .tdata/.tbss region is separate at kTlsStaticBase.

    struct Thr {
        std::uint32_t magic;
        std::uint32_t handle;
        std::uint64_t thread_ptr;
        std::uint64_t reent;
        std::uint64_t tls_tp;
    };
    static_assert(sizeof(Thr) == 0x20, "a ThreadLocalRegion tem 0x20 bytes no fim");

    // TP aponta para o TCB no fim da ThreadLocalRegion
    const std::uint64_t tp = layout::kTlsBase + 0x200;

    Thr t{};
    t.magic      = layout::kThreadVarsMagic;
    t.handle     = layout::kMainThreadHandle;
    t.thread_ptr = layout::kThreadObject;
    t.reent      = layout::kReentBase;
    t.tls_tp     = tp;

    // ThreadLocalRegion começa em kTlsBase (o message buffer está em offset 0)
    // O Thr struct fica no fim: kTlsBase + 0x200 - sizeof(Thr)
    const std::uint64_t tv = layout::kTlsBase + 0x200 - sizeof(Thr);
    if (!mem_.write(tv, &t, sizeof(t), error))
        return false;

    // O handle da thread, em TLS+0x110 (dentro do message buffer), é escrito
    // depois por quem conhece o handle verdadeiro: `SvcDispatcher::set_eden_tls`.
    //
    // Ver a nota longa em `SvcDispatcher::set_eden_tls` — o handle tem de ser
    // o da tabela de handles do Eden, e esse só existe depois de o sistema ser
    // montado. Escrever aqui um número diferente seria dar ao loader um handle
    // que não existe na tabela.

    // A região de TLS entregue ao guest tem de estar a zeros à partida. Não
    // é pprint: é o contrato — se houvesse lixo, o SDK ia ler um `magic`
    // errado e recusar a thread em vez de inicializar.
    //
    // O bloco já está dentro da reserva (que é um `mmap` anónimo, logo zeros),
    // por isso não se mapeia nada aqui. Zera-se à mesma, para que isto não
    // dependa de ser anónimo.
    if (!mem_.owns(layout::kTlsBase) ||
        !mem_.owns(layout::kTlsBase + 0x200 - 1)) {
        error = "ThreadLocalRegion fora da reserva do guest";
        return false;
    }
    std::vector<std::uint8_t> zeros(0x200, 0);
    if (!mem_.write(layout::kTlsBase, zeros.data(), zeros.size(), error))
        return false;

    // Re-escrever o Thr struct depois de zerar
    if (!mem_.write(tv, &t, sizeof(t), error))
        return false;

    // O bloco estático (.tdata/.tbss) também deve estar zerado
    if (!mem_.owns(layout::kTlsStaticBase) ||
        !mem_.owns(layout::kTlsStaticBase + layout::kTlsStaticSize - 1)) {
        error = "o bloco de TLS estático (" + hex(layout::kTlsStaticBase) +
                ") está fora da reserva do guest";
        return false;
    }
    std::vector<std::uint8_t> zeros_static(layout::kTlsStaticSize, 0);
    if (!mem_.write(layout::kTlsStaticBase, zeros_static.data(), zeros_static.size(), error))
        return false;

    // Os objectos que a ThreadLocalRegion aponta. O SDK desreferencia-os sem
    // os verificar, por isso têm de existir.'''

content = content.replace(old, new)
with open('src/guest_program.cpp', 'w') as f:
    f.write(content)
print('Done')