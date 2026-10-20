# Mali-G610 no RK3588 — porquê 850 MHz e como chegar a 1 GHz

Medido no Orange Pi 5 Pro (RK3588S, Mali-G610), kernel 6.18.38-current-rockchip64,
panthor, Mesa 25.2.8. Data: 2026-10-18.

## Resumo

**1 GHz é fisicamente possível neste SoC.** O limite de 850 MHz não é do
silício, nem térmico, nem do regulador: é aritmética de divisor de PLL no
caminho CRU que este kernel usa para a GPU.

> **Actualização após análise do kernel actual (18-10-2026).** A recomendação
> inicial ("portar o `rockchip_pvtm`") partia do pressuposto, agora refutado,
> de que o kernel era uma variante *vendor*. Não é. Ver §8.

## 1. O que está confirmado

### O tecto real

Pedir 1 GHz ao devfreq funciona; o relógio real não acompanha:

| `min_freq` pedido | `cur_freq` (sysfs) | **`clk_gpu` real** |
|---|---|---|
| 300 MHz | 300 000 000 | 300 000 000 |
| 400 MHz | 400 000 000 | 393 215 999 |
| 500 MHz | 500 000 000 | 500 000 000 |
| 600 MHz | 600 000 000 | 536 000 000 |
| 700 MHz | 700 000 000 | 536 000 000 |
| 800 MHz | 800 000 000 | 786 431 998 |
| 900 MHz | 900 000 000 | 850 000 000 |
| **1 000 MHz** | 1 000 000 000 | **850 000 000** |

`cur_freq` é o valor **pedido**, não o realizado. O round-down é silencioso.

### O que NÃO é bloqueio

| Possível bloqueio | Estado | Valor |
|---|---|---|
| Térmico | não | 42 °C, trip point a 85 °C, cooling em `cur_state=0` |
| Regulador | não | `vdd_gpu_s0`: 550–950 mV; 1 GHz pede 850 mV |
| Tabela OPP | não | tem `opp-1000000000` @ 850 mV neste kernel |
| Devfreq governor | não | `simple_ondemand`, mas `min_freq` é um floor de PM-QOS que ele não fura |
| Silício | não | a Mali-G610 é especificada até 1 GHz |

O OPP de 1 GHz existe na tabela *deste* kernel, o que por si só é evidência de
que o hardware foi projectado para o atingir.

## 2. A causa: divisores de PLL inteiros

`clk_gpu_src` é um **mux com divisor**. Do código do driver
(`drivers/clk/rockchip/clk-rk3588.c`):

```c
COMPOSITE(CLK_GPU_SRC, "clk_gpu_src", gpll_cpll_aupll_npll_spll_p, 0,
        RK3588_CLKSEL_CON(158), 5, 3, MFLAGS, 0, 5, DFLAGS, ...),
```

- `5, 3` → mux de 3 bits: escolhe um de 5 pais.
- `0, 5` → divisor de **5 bits**, valores 1..31, **inteiro**.

Confirmado em runtime por `/sys/kernel/debug/clk/clk_gpu_src/clk_possible_parents`:

```
clk_possible_parents = gpll cpll aupll npll spll
```

Taxas actuais desses pais:

| PLL | Taxa | Melhor resultado ≤ 1 GHz |
|---|---|---|
| gpll | 1608 MHz | ÷2 = 804 MHz |
| cpll | 1500 MHz | ÷2 = 750 MHz |
| aupll | 786,4 MHz | ÷1 = 786 MHz |
| **npll** | **850 MHz** | **÷1 = 850 MHz** ← máximo |
| spll | 702 MHz | ÷1 = 702 MHz |

Nenhum pai está a 1000 ou 2000 MHz, portanto **1000 MHz não é sintetizável por
divisão inteira a partir de nenhum deles**. O melhor resultado possível é
850 MHz — exactamente o que foi medido.

> Nota: a documentação anterior do projecto dizia que o limite vinha do NPLL a
> 850 MHz. Isso está incompleto: o NPLL é de facto o pai que dá o melhor
> resultado, mas o limite não é o NPLL em si — é que **nenhum** dos cinco pais
> permite obter 1000 MHz com o divisor inteiro disponível.

## 3. Confirmação empírica: a imagem que o utilizador já usou

Uma imagem de sistema que o utilizador confirmou rodar a 1 GHz foi
descomprimida e analisada:

```
/home/trebor/Documentos/backup_OPI5pro_perfeito_2026-07-24.img.zst
  → Armbian-unofficial 26.08.0-trunk jammy
  → kernel: 6.1.115-vendor-rk35xx          <-- kernel VENDOR Rockchip
```

O DTB do Orange Pi 5 Pro nessa imagem (`rk3588s-orangepi-5-pro.dtb`) tem a
tabela OPP completa, **com 1 GHz**:

```dts
gpu_opp_table: gpu-opp-table {
    rockchip,supported-hw;
    rockchip,pvtm-pvtpll;               /* <— o clock vem do PVTPLL */
    rockchip,pvtm-voltage-sel = <0x00 0x32f 0x00
                                 0x330 0x343 0x01
                                 0x344 0x35c 0x02
                                 0x35d 0x375 0x03
                                 0x376 0x38e 0x04
                                 0x38f 0x270f 0x05>;   /* 911 mV+ */
    rockchip,opp-clocks = <&cru CLK_GPU>;
    ...
    opp-1000000000 {
        opp-supported-hw = <0xf9 0xffff>;
        opp-hz      = <0x00 0x3b9aca00>;              /* 1 000 000 000 */
        opp-microvolt = <0xcf850 ...>;                 /* 850 000 µV = 850 mV */
        /* mais 5 níveis, selected pela temperatura */
    };
};
```

Decisivo — **na imagem que funcionava a 1 GHz**:

```
/lib/modules/6.1.115-vendor-rk35xx/kernel/drivers/soc/rockchip/rockchip_pvtm.ko   ← PRESENTE
/lib/modules/6.1.115-vendor-rk35xx/kernel/drivers/firmware/arm_scmi/scmi-module.ko ← PRESENTE
```

**No kernel actual deste Pi:**

```
find /lib/modules -name "*pvtm*"    → vazio        ← rockchip_pvtm AUSENTE
```

Isto fecha o caso. Não é o silício, não é o regulador, não é o OPP table, não
é o divisor — é que **o kernel actual não tem o driver `rockchip_pvtm`**, que
é o que programa o PVTPLL e permite sintetizar 1000 MHz.

## 4. O kernel vendor resolve isto (PVTPLL)


No `rk3588s.dtsi` do kernel Rockchip (`rk-6.12-rkr1`):

```dts
gpu_opp_table: gpu-opp-table {
    rockchip,opp-clocks = <&cru CLK_GPU>;
    rockchip,pvtm-pvtpll;              /* clock vem do PVTPLL */
    rockchip,pvtm-hw = <0x04>;
    ...
    opp-1000000000 {                  /* 1 GHz existe na tabela */
        opp-supported-hw = <0xe9 0xffff>;
        opp-hz = /bits/ 64 <1000000000>;
        opp-microvolt = <850000 850000 950000>;
    };
};
```

O `PVTPLL` não passa pelo mux CRU com divisor inteiro, por isso consegue
sintetizar os 1000 MHz.

Confirmação adicional de que a GPU usa o SCMI neste desenho:

```dts
gpu: gpu@fb000000 {
    clocks = <&scmi_clk SCMI_CLK_GPU>, ...;
    assigned-clocks = <&scmi_clk SCMI_CLK_GPU>;
};
```

E no kernel actual o `scmi_clk_gpu` **já existe** na árvore de clock
(`/sys/kernel/debug/clk/clk_summary`), com `clk_gpu_pvtm` (o monitor PVT) a
24 MHz. A infraestrutura está lá; o que falta é o wiring no DT e o driver
PVTM.

### A. Kernel Rockchip vendor/rkr com `rockchip_pvtm` — *o caminho comprovado*

**É a configuração em que o utilizador já chegou a 1 GHz.** O kernel
`6.1.115-vendor-rk35xx` da imagem de referência traz
`drivers/soc/rockchip/rockchip_pvtm.ko` e o DTB com `rockchip,pvtm-pvtpll` e o
OPP de 1 GHz a 850 mV.

Branches do `armbian/linux-rockchip` com o mesmo driver: `rk-6.1-rkr5.1`,
`rk-6.12-rkr1`.

- **Prós**: caminho já validado em hardware por quem fez. Inclui o OPP de
  1 GHz, a voltagem correcta e os níveis de voltagem por temperatura.
- **Contras**: kernel diferente do actual (6.1 vs 6.18). Tem de se confirmar que
  panthor, PanVK e o EmuOS continuam a funcionar. E é um downgrade de kernel —
  o EmuOS pode estar afinado ao 6.18.

### B. Portar só o `rockchip_pvtm` para o 6.18

Em vez de trocar o kernel inteiro, portar `rockchip_pvtm.ko` do 6.1 para o 6.18
e usar o DTB da imagem de referência com o kernel actual.

- **Prós**: mantém o 6.18 e todo o resto do EmuOS intacto; o ganho (+17,6%) é
  o mesmo.
- **Contras**: trabalho de portabilidade (dependências de API do clock/regulator
  entre 6.1 e 6.18; provavelmente algumas horas e uma árvore de kernel local).

### C. Patch de DTB no kernel actual

Usar o DTB da imagem de referência com o kernel 6.18.

- **Prós**: o DTB já está extraído e é o que funcionava.
- **Contras**: **sozinho não chega.** Sem `rockchip_pvtm` no kernel, o DT
  declara o PVTPLL mas não há nada para o programar. Tem de ser combinado com
  A ou B.

### D. Reprogramar um PLL CRU — *não recomendado*

Mudar o `spll` ou o `npll` por escrita directa.

- `spll` alimenta o clock de reshape/bus; `npll` alimenta a interconnect
  central (`aclk_center_*`, `hclk_center_root`) e daí PCIe, CSI e USB. Mexer
  nisto pode destabilizar o sistema.
- Não resolve a causa: apenas muda qual o pai que dá 850 MHz.

## 5. Recomendação

**B — portar o `rockchip_pvtm` para o 6.18**, com o DTB da imagem de
referência.

É o melhor rácio: dá os mesmos +17,6% que a opção A, sem downgrade de kernel
e sem mexer no resto do EmuOS. O driver é um `.ko` isolado em
`drivers/soc/rockchip/`, com dependências estreitas (devfreq + regulator +
regmap), o que o torna portável ao contrário do que à primeira vista parece.

Alternativa pragmática: **A**, se a portabilidade revelar problemas e o
EmuOS sobreviver ao 6.1.


## 6. Impacto esperado

850 → 1000 MHz é **+17,6% de clock**.

A Mali-G610 é uma tiler de **fill-rate** alto e largura de banda baixa, e o
Pokémon Shield é limitado por fill rate. Neste caso o ganho de clock
translaciona-se praticamente 1:1 em FPS. Acompanhar com o `gpu_ns` da
telemetria do EdenMaliG610 dá a confirmação directa: se o FPS subir ~17%, o
gargalo era mesmo a GPU e o clock era mesmo o travão.

## 7. Como confirmar que se chegou lá

Não basta `cur_freq` dizer 1000000000 — o round-down mente. Tem de se ler o
relógio real:

```sh
sudo awk '$1=="clk_gpu"{printf "%.0f MHz\n", $5/1e6}' /sys/kernel/debug/clk/clk_summary
```

E confirmar que não há throttling:

```sh
cat /sys/class/devfreq/fb000000.gpu/cooling_device/cur_state   # 0 = sem throttling
```

---

## 8. Actualização: o kernel actual é mainline, não vendor

A análise inicial comparou o DTB da imagem que funcionava (kernel vendor
`6.1.115-vendor-rk35xx`, com `rockchip_pvtm.ko`) com o kernel actual, e
concluiu "falta o driver, portemo-lo". **Essa conclusão estava errada**, e a
distinção muda a recomendação.

### O que o kernel actual é

```
$ uname -r
6.18.38-current-rockchip64
$ cat /proc/version
Linux version 6.18.38-current-rockchip64 (build@armbian) (aarch64-linux-gnu-gcc ...)
```

Compilado pela Armbian a partir de **mainline** (`torvalds/linux`), não do
kernel vendor da Rockchip. Confirmado por comparação directa:

| | vendor (imagem que funcionava) | actual |
|---|---|---|
| base | rockchip-linux fork | torvalds/linux mainline |
| versão | 6.1.115-vendor-rk35xx | 6.18.38-current-rockchip64 |
| `rockchip_pvtm` | presente | **ausente** |
| PVTPLL exposto | sim | **não** |

### Porquê o mainline tem o mesmo tecto

O `CLK_GPU_SRC` no mainline 6.18 é **byte-a-byte o mesmo** que no vendor:

```c
// torvalds/linux v6.18, drivers/clk/rockchip/clk-rk3588.c:1354
COMPOSITE(CLK_GPU_SRC, "clk_gpu_src", gpll_cpll_aupll_npll_spll_p, 0,
        RK3588_CLKSEL_CON(158), 5, 3, MFLAGS, 0, 5, DFLAGS, ...),
PNAME(gpll_cpll_aupll_npll_spll_p) = { "gpll", "cpll", "aupll", "npll", "spll" };
```

O mainline só tem os **gates** PVTM (o monitor de 24 MHz), nunca o PVTPLL:

```
$ grep -iE "pvtpll|pvtm" clk-mainline.c
801: GATE(PCLK_BIGCORE0_PVTM, ...)     # só gates do monitor
1364: GATE(CLK_GPU_PVTM, "clk_gpu_pvtm", "xin24m", 0, ...)
```

### Consequência: portar o driver é o caminho caro

`rockchip_pvtm.c` (1046 linhas na 6.1, 932 na 6.12) depende de infra-estrutura
que **não existe em mainline**:

| Dependência | Mainline 6.18 |
|---|---|
| `include/linux/soc/rockchip/pvtm.h` | ausente |
| PVTPLL exposto ao clock framework | ausente |
| `rockchip,pvtm-*` no DT | ausente |
| SCMI clock para a GPU no DT da GPU | ausente |
| `rockchip_opp_select.c` (selecção por leakage nvmem) | ausente |

Não é portar um módulo: é portar um driver **e a sua infra-estrutura**, para um
PLL que o mainline não expõe sequer. O ganho (+17,6%) não justifica esse custo.

### O caminho real que resta

O mainline **tem** SCMI funcional, e o `scmi_clk_gpu` **está registado**:

```
$ ls /sys/bus/scmi_protocol/devices/
scmi_dev.3 -> driver: scmi-clocks        # dispositivo de clocks do firmware

$ grep scmi /sys/kernel/debug/clk/clk_summary
scmi_clk_gpu    0 0 0 300000000 ...       # existe e responde
scmi_clk_npu    0 0 0 200000000 ...
scmi_clk_ddr    0 0 0 2400000000 ...
```

O `clk_scmi` é **builtin** (`filename: (builtin)`), não um módulo a portar.

O bloqueio é que o **DT deste tabuleiro aponta a GPU para o CRU**, não para o
SCMI:

```
$ tr '\0' '|' < /proc/device-tree/gpu@fb000000/clock-names
core|coregroup|stacks|          # sem entrada scmi

$ od -A d -t x4 /proc/device-tree/gpu@fb000000/clocks
phandles: 0x21(cru) 0x106 0x21(cru) 0x107 0x21(cru) 0x108   # todos CRU
```

O DTB vendor, pelo contrário, usa:

```dts
clocks = <&scmi_clk SCMI_CLK_GPU>, <&cru CLK_GPU_COREGROUP>,
         <&cru CLK_GPU_STACKS>, <&cru CLK_GPU>;
```

### Caminhos reavaliados

Em ordem de custo/interferência crescentes:

**1. DTB: apontar a GPU para `scmi_clk_gpu`.** Altera só o device tree.
Reversível com um reboot. Se o firmware aceitar 1 GHz por SCMI, resolve sem
qualquer código. **É o primeiro a testar.** O DTB correcto já está extraído
neste projecto (`docs/rk3588s-orangepi-5-pro.dtb`, da imagem que funcionava).

**2. Módulo pequeno com `clk_set_rate()` sobre `scmi_clk_gpu`.** Se o caminho 1
não EXPUSER o clock mas o firmware aceitar a taxa, um módulo de ~50 linhas
basta — usa a API de clocks do kernel, que já existe.

**3. Reprogramar o `spll` no CRU.** O `spll` é o PLL mais isolado do SoC:
alimenta apenas `clk_dsihost0/1`, `clk_isp0_core`, `clk_fisheye*` e
`aclk_jpeg_decoder`, **todos com `enable_count=0`**. Se for reprogramado para
2000 MHz, então `2000/2 = 1000 MHz` e o divisor inteiro passa a servir.

```bash
$ grep -A12 "^ *spll " /sys/kernel/debug/clk/clk_summary
 spll   2 3 0 702000000 ... Y
    clk_dsihost1    0 0 0 351000000 ... N
    clk_dsihost0    0 0 0 351000000 ... N
    clk_isp0_core   0 0 0 702000000 ... N
    clk_fisheye1_core ...                     N
    aclk_jpeg_decoder_root ...                N
```

**Bloqueio para o caminho 3:** `CONFIG_STRICT_DEVMEM=y` neste kernel, logo
`/dev/mem` recusa a leitura (`Bad address`) e não há forma de escrever no CRU
sem recompilar o kernel. Um módulo `kernel` faz `ioremap()` do CRU — é
portanto também via código.

### Previsão de resultado

Os caminhos 1 e 2 dependem de uma coisa que **não se pode determinar sem
experimentar**: se o firmware SCP aceita 1000 MHz para `SCMI_CLK_GPU`. A
documentação da Rockchip e a existência do OPP de 1 GHz a 850 mV suggestem que
sim, mas é exactamente por isso que é um teste e não uma conclusão.

O caminho 3 não depende do firmware e é mecanicamente certo, mas é o que mais
pode destabilizar o sistema se a escrita ao CRU for mal calculada.
