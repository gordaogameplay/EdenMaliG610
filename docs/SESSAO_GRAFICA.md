# Desactivar o sway — medido, revertido, e porque não é a solução

**Data:** 2026-10-18
**Estado final:** inalterado. O patch fica instalado mas **inactivo**.

## O que foi feito

`emuos-session.sh` tem um gancho reversível: se `/run/emuos/no-sway` existir,
`start_sway()` devolve 1 e a sessão cai no modo legado.

```bash
# activar
sudo touch /run/emuos/no-sway
sudo systemctl restart emuos-session

# reverter
sudo rm /run/emuos/no-sway
sudo systemctl restart emuos-session
```

Backup do original: `/usr/local/bin/emuos-session.sh.bak-20261018`
(sha256 `69e277e3c49e723668e19fa8aa9b7dc995ff2ca758839a268c925a1f91f6f344`).

O marcador está em `/run` (tmpfs) e some no reboot — um esquecimento não custa
nada. E é um **desvio**, não uma remoção: nenhuma linha foi apagada.

## Custo medido do sway

```
PID   %CPU   RSS     COMMAND
833   4.0    113 MB  sway
898   0.0    5.4 MB  swaybg
```

4% de **um** núcleo = ~0,5% dos 8. O sistema tem 90% de idle. Para testes de
execução nativa, isto é irrelevante.

O menu custa mais que o sway:

```
98821  23.7  130 MB  python3 /usr/local/bin/emuos-menu
```

Isto é a medição que contraria a premissa: **o alvo certo não é o sway, é o
menu.**

## Porque foi revertido: o modo legado está partido

Com o sway desligado, a sessão cai em eglfs/KMSDRM — e neste device isso não
funciona:

```
[09:07:28] sway desactivado pelo marcador; modo legado
[09:07:28] modo: legado (KMSDRM)
[menu] renderer=acelerado
ERROR: Could not restore CRTC
ERROR: Could not set videomode on CRTC.
ERROR: Could not queue pageflip: -16      <- repete-se sem parar
```

`EBUSY` em pageflip, o menu entra em ciclo de tentativas:

- **92% de CPU** (contra os 4% do sway) — pior, não melhor
- **17 KB/s** de log, 1,5 MB por minuto — o log enche o disco
- a tela fica preta, que é o oposto do que se queria

O motivo está no próprio `emuos-session.sh`, no cabeçalho: o modo legado é
descrito como o *fallback* para quando o sway não sobe (sem monitor no DP,
problema de driver). Está lá para o console nunca ficar morto, não para ser um
modo de operação normal.

**Conclusão medida: desligar o sway não liberta CPU — troca 4% por 92% e um
ecrã preto.** O patch fica instalado e inactivo; a decisão de desligar o
sway é do utilizador, com este custo medido à vista.

## O que libertaria CPU de verdade

Por ordem de custo/risco, para testes NCE:

| Acção | Ganho | Risco |
|---|---|---|
| parar o `emuos-session` | ~28% (sway + menu) | perder o menu na TV |
| parar só o menu | ~24% | perder o menu |
| reduzir `vblank` do sway | ~2% | nenhum |
| **parar a sessão** | o maior | é o correcto para SSH |

Para testes de execução nativa via SSH, a sessão gráfica não é necessária. O
alvo natural é `systemctl stop emuos-session` durante a medição, e voltar a
arrancar depois. Isso liberta os ~28% sem depender do modo legado.
