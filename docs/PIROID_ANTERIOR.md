# O runtime anterior do piDroid não executava o jogo

**Data:** 2026-10-18
**Ficheiro:** `~/piDroid/piDroid_Test_ARM/runtime/src/main_loader.cpp`

## O que o log mostrava

```
[LOADER] Sucesso: 36 MB alocados em buffer.
[SVC PROXY] Interceptado: svcControlMemory -> Tamanho: 32768 KB
[IPC KERNEL] Requisição de conexão recebida para a porta: nvdrv
 -> [MAPPED] Conectado ao subsistema do Driver de GPU
[VI SERVICE] Display padrao inicializado com sucesso (1280x720/1920x1080)
[NVDRV IOCTL] Comando recebido: 0xc0184401
[GPU BRIDGE] Interceptado SwapBuffers -> Desenhando buffer de textura 0xabcde001
[SUCCESS] Ambiente de execucao e aceleracao ativados.
```

Lê-se como um arranque bem-sucedido: nvdrv ligado, display aberto, comandos
enviados à GPU, Vulkan instanciado.

## O que o código faz

```cpp
int main(int argc, char** argv) {
    HorizonLoader loader(elf_path, npdm_path);

    if (loader.ler_manifesto_npdm()) {
        loader.carregar_binario_para_ram();     // lê o ficheiro para memória
        loader.alocar_ambiente_virtual();      // aloca um buffer
        disparar_bootstrap_grafico();          // imprime linhas de log
        disparar_bootstrap_video();            // imprime linhas de log
        disparar_bootstrap_filesystem();       // imprime linhas de log
        disparar_bootstrap_audio();            // imprime linhas de log
        testar_envio_de_comandos_gpu(10);      // imprime linhas de log
        disparar_render_bridge();              // imprime linhas de log
        std::cout << "[SUCCESS] ..." << std::endl;
        while (true) { sleep(1); }
    }
```

**Não há execução de guest em lado nenhum.** As sete funções `disparar_*` são
`std::cout` com handles fixos (`fs_handle = 12`, `aud_handle = 13`). A ponte de
renderização é chamada com um identificador de textura inventado:

```cpp
gpu_bridge.processar_swap_buffers(0xABCDE001);
```

E o programa termina num `while (true) { sleep(1); }`.

## Como se confirma

```
$ grep -rn "abcde001"              runtime/src -> 0 ocorrências
$ grep -rn "Interceptado SwapBuffers" runtime/src -> 0 ocorrências
$ grep -rn "MAPPED"                runtime/src -> 0 ocorrências
$ grep -rn "500x2d0"               runtime/src -> 0 ocorrências
```

As strings **só existem no ficheiro de log**, não no código que o produz. O
`[SUCCESS]` existe no código — mas é um `std::cout` a seguir a sete chamadas que
só imprimem.

## Porque isto importa

**Não há herança técnica a recuperar.** O que parecia "o jogo já corria até
ao ponto X" era um log. Isto muda o ponto de partida:

| acreditava-se | realidade |
|---|---|
| o jogo arrancava | `sleep(1)` infinito |
| nvdrv comunicava | `std::cout` |
| Vulkan instanciado a 60 Hz | `MaliVulkanBridge(1280, 720)` + log |
| GPU a desenhar | textura `0xABCDE001`, um literal |

A única coisa que o runtime anterior fez de verdade foi carregar o binário num
buffer — exactamente o que o nosso `NsoLoader` já fazia, e mais correctamente
(imagem no espaço virtual, com os gaps respeitados).

## O que se aproveita

O `HorizonLoader` e as estruturas de serviços são um esboço útil como
**especificação** do que a interface tem de expor: `nvdrv`, VI, FSP-SRV,
AUDREN, com os handles e a ordem de bootstrap. Isso é informação útil sobre o
protocolo — não implementação.

E os erros do runtime anterior são exactamente os erros que já encontrámos e
corrigimos no EdenMaliIA:

- **`svcControlMemory`** — número de SVC que não existe. O nosso era
  `SetMemoryAttribute`; o correcto é `SetMemoryPermission` = `0x02`.
- **load de 36 MB** — a soma dos segmentos, que ignora os gaps. É o bug que
  fazia a `.rodata` aterrar 3328 bytes deslocada.
- **sem `x18`** — sem platform register, o primeiro `__stack_chk_fail` do guest
  rebenta.

Ou seja: o log anterior não era só vazio. Era um registo de uma implementação
com os mesmos bugs de arquitectura que acabámos de encontrar, escrito de forma
a parecer sucesso.
