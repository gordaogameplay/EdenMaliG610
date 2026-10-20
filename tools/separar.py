#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Separa a historia do projecto em commits por assunto.

Como funciona
--------------
Trabalha com **conteudo**, nao com patches. Para cada commit da historia:

  * cada **fatia** diz que hunks de que ficheiros entram nela;
  * o conteudo de cada ficheiro e' reconstruido a partir do conteudo do
    pai, com os hunks escolhidos aplicados;
  * a soma das fatias tem de dar o conteudo do commit original — e a
    prova e' que a arvore final e' identica a do HEAD original.

Por que conteudo e nao patches: um patch partido ao meio e' fragil (o
contexto tem de estar na posicao certa, e as contagens de linhas mudam).
O texto nao tem nenhum desses problemas.

O ficheiro JSON com o plano:

    {
      "7c08795": [
        {"msg": "...", "body": "...",
         "files": ["a", "b"],                  # ficheiros inteiros
         "hunks": {"src/x.cpp": [0, 2]},       # so esses hunks
         "append": {"PROGRESSO.md": [1, 40]}}, # linhas do bloco acrescentado
        ...
      ]
    }

O `hunks` aceita tres formas:
    [0, 2]                    -> os hunks 0 e 2, inteiros
    {"0": [[1, 1]], "2": [3]}  -> o hunk 0 partido (1a linha nova) e o
                                  hunk 2 inteiro
"""

import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.environ.get("SPLIT_REPO",
                     os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# O indice temporario. O indice real do repositorio nao e' tocado: e' o
# `read-tree` que decide o estado, e um `read-tree` no indice real
# choca com os ficheiros que estao no disco.
INDICE = os.environ.get("SPLIT_INDEX", "/tmp/_split_index")


def git_indice(*args, **kw):
    """Um `git` que usa o indice temporario."""
    env = dict(os.environ)
    env["GIT_INDEX_FILE"] = INDICE
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True, cwd=REPO, env=env, **kw)


def git(*args, **kw):
    # As operacoes normais usam o indice **real**: o
    # `GIT_INDEX_FILE` do ambiente aponta para o temporario, e um
    # `rev-parse` ou um `diff` sobre ele da' resultados diferentes.
    env = dict(os.environ)
    env.pop("GIT_INDEX_FILE", None)
    kw.setdefault("env", env)
    return subprocess.run(["git"] + list(args), capture_output=True, text=True,
                          cwd=REPO, **kw)


def check(r, *args):
    if r.returncode != 0:
        print("git %s falhou:\n%s\n%s" % (" ".join(args), r.stdout, r.stderr),
              file=sys.stderr)
        sys.exit(1)
    return r.stdout


def conteudo(rev, caminho=None):
    """O conteudo de um caminho numa revisao. Devolve None se nao existir.

    A revisao "corrigido" le o ficheiro do disco: e' para os ficheiros
    que foram corrigidos depois do commit original.
    """
    if rev == "corrigido":
        with open(os.path.join(REPO, caminho), encoding="utf-8") as f:
            return f.read()
    if rev.startswith(":"):
        env = dict(os.environ)
        env["GIT_INDEX_FILE"] = INDICE
        r = subprocess.run(["git", "show", rev], capture_output=True,
                           cwd=REPO, env=env)
    else:
        r = subprocess.run(["git", "show", "%s:%s" % (rev, caminho)],
                           capture_output=True, cwd=REPO)
    if r.returncode != 0:
        return None
    return r.stdout.decode("utf-8", "replace")


def diff_bruto(parent, commit, path):
    r = subprocess.run(["git", "diff", "-U3", "--no-color", "--no-ext-diff",
                        parent, commit, "--", path],
                       capture_output=True, cwd=REPO)
    return r.stdout


def dividir_hunks(bruto):
    """Separa o diff em (cabecalho, [hunks])."""
    texto = bruto.decode("utf-8", "replace")
    linhas = texto.split("\n")
    cabecalho, hunks = [], []
    i = 0
    while i < len(linhas):
        l = linhas[i]
        if l.startswith("@@"):
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$", l)
            corpo = [l]
            i += 1
            while i < len(linhas) and not linhas[i].startswith("@@") \
                    and not linhas[i].startswith("diff --git"):
                corpo.append(linhas[i])
                i += 1
            hunks.append({
                "old": int(m.group(1)),
                "oldn": int(m.group(2) or 1),
                "new": int(m.group(3)),
                "newn": int(m.group(4) or 1),
                "linhas": corpo,
            })
            continue
        if not hunks:
            cabecalho.append(l)
        i += 1
    return cabecalho, hunks


class NaoCasa(Exception):
    """O contexto de um hunk nao casou no ficheiro."""


def aplicar_hunks(texto, hunks, selecao):
    """Aplica varios hunks a um texto, **de baixo para cima**.

    Cada hunk e' procurado pelo seu **contexto**, e nao pela posicao: um
    hunk anterior pode ter deslocado as linhas.

    E a ordem e' de baixo para cima porque e' a que nao destroi: um hunk
    mais abaixo nao affected os acima, porque os numeros de linha sao
    relativos ao ficheiro **antes** de qualquer aplicacao. Aplicados de
    cima para baixo, o segundo hunk procuraria o seu contexto num ficheiro
    que ja mudou — e a procura falharia.

    O resultado e' o mesmo em qualquer ordem, porque os hunks de um
    ficheiro nao se sobrepoem.
    """
    # **De baixo para cima.** Um hunk mais abaixo nao affect os acima:
    # os numeros de linha sao relativos ao ficheiro **antes** de qualquer
    # aplicacao. Aplicados de cima para baixo, o segundo hunk procuraria
    # o seu contexto num ficheiro que ja mudou.
    for i, pedacos in sorted(selecao, reverse=True):
        texto = aplicar_hunk(texto, hunks[i], pedacos)
    return texto


def aplicar_hunk(texto, hunk, pedacos):
    """Aplica um hunk ao texto, so as linhas novas indicadas.

    `pedacos` e' None (o hunk inteiro) ou uma lista de (inicio, fim)
    1-based sobre as linhas **novas** do hunk.

    A ideia: o hunk e' uma sequencia de itens (contexto, remocao, nova).
    O ficheiro tem o **bloco antigo** (contexto + remocoes). O resultado
    tem o **bloco novo** (contexto + as linhas novas escolhidas).

    O bloco antigo e' procurado no ficheiro pelo seu **contexto** — que
    e' estavel, ao contrario dos numeros de linha, que uma fatia
    anterior pode ter deslocado.

    Devolve o texto novo, ou levanta `NaoCasa`.
    """
    corpo = hunk["linhas"][1:]

    houve_marcador = False
    itens = []
    for l in corpo:
        if l.startswith("+"):
            itens.append(("+", l[1:]))
        elif l.startswith("-"):
            itens.append(("-", l[1:]))
        elif l.startswith(" "):
            itens.append((" ", l[1:]))
        elif l == "":
            # Uma linha vazia no diff pode ser duas coisas: uma linha
            # **vazia do ficheiro** (o ficheiro tem essa linha), ou o
            # separador que o `git diff` poe entre os ficheiros.
            #
            # A distincao e' o `oldn` do cabecalho: se o hunk consome
            # mais linhas do que as contadas, a ultima linha vazia e'
            # uma linha do ficheiro.
            pass
        elif l.startswith("\\"):
            # "\ No newline at end of file": **nao e' uma linha do
            # ficheiro**. E' o marcador que diz que o ficheiro acaba
            # sem "\n".
            houve_marcador = True
            pass
        else:
            itens.append((" ", l))

    # os indices das linhas novas e das remocoes
    idx_novas = [i for i, (k, _) in enumerate(itens) if k == "+"]
    idx_remocoes = [i for i, (k, _) in enumerate(itens) if k == "-"]

    # As faixas do hunk: os intervalos do **corpo** (1-based). Uma
    # faixa e' um intervalo de linhas do diff — o contexto Included, e
    # por isso que o hunk partido casa.
    faixas = None
    if isinstance(pedacos, dict) and "h" in pedacos:
        faixas = pedacos["h"]
        pedacos = None

    if pedacos is None and faixas is None:
        escolhidas = list(range(len(idx_novas)))
    elif faixas is not None:
        escolhidas = []
        for ini, fim in faixas:
            for j, pos in enumerate(idx_novas):
                # a posicao no corpo do hunk: o cabecalho e' a linha 0
                if ini <= pos + 1 <= fim:
                    escolhidas.append(j)
    else:
        escolhidas = []
        for ini, fim in pedacos:
            escolhidas.extend(range(ini - 1, fim))

    # As remocoes que saem: as que estao **dentro** do intervalo
    # escolhido, e as que a adicao escolhida substitui.
    #
    # E' o par implicito do diff: o `diff` poe a remocao e a adicao lado
    # a lado, e partir o hunk ao meio de um par deixa o codigo partido ao
    # meio.
    #
    # A regra: uma remocao sai se a proxima linha **nova** (sem contexto
    # pelo meio) foi escolhida.
    removidas_set = set()
    for pos, (k, _) in enumerate(itens):
        if k != "-":
            continue
        if faixas is not None:
            # dentro de uma das faixas?
            dentro = any(ini <= pos + 1 <= fim for ini, fim in faixas)
            if dentro:
                removidas_set.add(pos)
            continue
        prox = None
        for q in range(pos + 1, len(itens)):
            if itens[q][0] == "+":
                prox = q
                break
            if itens[q][0] == " ":
                break
        if prox is not None and prox in set(idx_novas[j] for j in escolhidas):
            removidas_set.add(pos)

    # ── o resultado ──
    #
    # Percorre-se o hunk uma vez, e cada item vai para o sitio certo:
    #
    #   * contexto antes do bloco  -> `antes`
    #   * contexto dentro do bloco -> `meio` (mantem a ordem)
    #   * remocao dentro do bloco -> desaparece
    #   * linha nova escolhida    -> `novas`
    #   * linha nova nao escolhida-> desaparece
    #   * contexto depois do bloco-> `depois`
    #
    # O contexto do meio e' o que impede o bloco antigo de casar: uma
    # remocao seguida de contexto e' um par que o ficheiro tem de ter.

    if escolhidas:
        primeiro = idx_novas[escolhidas[0]]
        ultimo = idx_novas[escolhidas[-1]]
    else:
        primeiro = len(itens)
        ultimo = len(itens) - 1

    # O resultado: o hunk percorrido uma vez, com as remocoes a
    # desaparecer e as linhas novas escolhidas a entrar no lugar delas.
    #
    # O contexto **depois** do bloco tambem entra — porque o bloco
    # antigo o consome, e o resultado tem de o repor.
    escolhidas_set = set(idx_novas[j] for j in escolhidas)
    # As remocoes que saem: as que estao **dentro** do intervalo
    # escolhido. Uma remocao fora do intervalo fica — porque a fatia que
    # a remove e' outra.
    #
    # (Sem isto, partir o hunk deixava a linha antiga **e** a nova.)

    resultado = []
    for i, (k, t) in enumerate(itens):
        if k == " ":
            resultado.append(t)
        elif k == "-":
            if i in removidas_set:
                pass      # a remocao desaparece
            else:
                resultado.append(t)   # fica: e' de outra fatia
        elif k == "+":
            if i in escolhidas_set:
                resultado.append(t)

    # ── a procura do bloco antigo ──
    #
    # O bloco antigo e' o contexto + as remocoes, **na ordem em que
    # aparecem no hunk**. As remocoes que estao *dentro* do bloco
    # escolhido tem de ser contadas, porque o ficheiro as tem.
    #
    # A ancora e' o contexto do principio do hunk (as primeiras linhas de
    # contexto), que e' o mais estavel: um hunk comeca sempre por
    # contexto, e esse contexto raramente muda.
    linhas = texto.split("\n")
    fim = len(linhas)
    while fim > 0 and linhas[fim - 1] == "":
        fim -= 1

    # o bloco antigo: contexto e remocoes, ate ao fim do bloco escolhido
    # (ou ate ao fim do hunk, se nao houver remocoes dentro)
    # O bloco antigo: o contexto e as remocoes, ate ao fim da **ultima
    # remocao** que esta dentro do bloco escolhido. Se nao houver
    # remocao dentro, o bloco antigo e' so o contexto ate ao fim do
    # bloco escolhido.
    #
    # E' esta regra que evita a duplicacao: o contexto que esta no meio
    # do bloco (depois de uma remocao, antes da linha nova) sai do
    # ficheiro com o bloco antigo e entra no sitio novo com o
    # resultado — uma vez so.
    # O bloco antigo e' o hunk inteiro, visto pelo lado do ficheiro
    # antigo: o contexto e as remocoes.
    #
    # E o `resultado` e' o hunk inteiro, visto pelo lado do ficheiro
    # novo: o contexto e as linhas novas escolhidas.
    #
    # **Os dois tem de ter o mesmo alcance** — senao o contexto fica
    # duplicado, ou falta. Por isso o bloco antigo vai ate ao **ultimo
    # item** do hunk, e o resultado tambem.
    # O bloco antigo: o contexto e as remocoes. **As remocoes que ficam**
    # (porque sao de outra fatia) tambem fazem parte — o ficheiro tem-nas.
    bloco_antigo = [itens[i][1] for i in range(0, len(itens))
                    if itens[i][0] in (" ", "-")]
    # O cabecalho diz quantas linhas o hunk consome do ficheiro antigo.
    #
    # Se o `bloco_antigo` tiver **mais**, a ultima linha vazia do diff e'
    # uma linha **vazia do ficheiro** — e faz parte do bloco. (O `diff`
    # poe uma linha "" no fim de cada ficheiro, e ela conta.)
    #
    # Se tiver **menos**, e' porque uma remocao saiu (esta fatia remove
    # a linha, e o pai nao a tinha) — e o bloco antigo e' menor de
    # proposito.
    if len(bloco_antigo) > hunk["oldn"]:
        bloco_antigo = bloco_antigo[:hunk["oldn"]]

    pos = hunk["old"] - 1
    if (pos < 0 or pos + len(bloco_antigo) > fim
            or linhas[pos:pos + len(bloco_antigo)] != bloco_antigo):
        pos = -1
        for p in range(0, fim - len(bloco_antigo) + 1):
            if linhas[p:p + len(bloco_antigo)] == bloco_antigo:
                pos = p
                break
        if pos < 0:
            raise NaoCasa("o contexto nao casa: esperado %r" % (bloco_antigo[:3],))

    novas_linhas = linhas[:pos] + resultado + linhas[pos + len(bloco_antigo):]
    saida = "\n".join(novas_linhas)
    # O fim do ficheiro.
    #
    # Alguns ficheiros nao acabam em "\n", e o diff respeita isso (com o
    # marcador "\ No newline at end of file"). A regra e' simples: o
    # resultado herda o fim **do commit**, e nao o do pai — porque e' o
    # commit que diz como o ficheiro fica.
    #
    # Mas o `aplicar_hunk` nao conhece o commit. A solucao: o fim
    # herda o do texto de entrada, salvo se o hunk tiver o marcador
    # "\ No newline at end of file" — e nesse caso o fim **e'** o do
    # resultado.
    if houve_marcador:
        if not saida.endswith("\n"):
            saida += "\n"
    else:
        if texto.endswith("\n") and not saida.endswith("\n"):
            saida += "\n"
        elif not texto.endswith("\n") and saida.endswith("\n"):
            saida = saida[:-1]
    return saida


def escrever_indice(caminho, texto):
    """Poe um conteudo no indice."""
    tmp = "/tmp/_split_blob"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(texto)
    r = git_indice("hash-object", "-w", tmp)
    check(r, "hash-object")
    blob = r.stdout.strip()
    r = git_indice("update-index", "--add", "--cacheinfo",
                   "100644,%s,%s" % (blob, caminho))
    check(r, "update-index")


def aplicar_fatia(parent, commit, fatia, primeiro, compartilhados=frozenset(),
                   partida=None, fatias_deste_commit=()):
    """Aplica uma fatia ao indice.

    Cada ficheiro e' tentado **duas vezes**: uma com o conteudo do pai, e
    outra com o estado acumulado. A primeira e' a correcta na maioria dos
    casos; a segunda e' a necessaria quando o contexto de um hunk depende
    de uma fatia anterior.
    """
    if primeiro:
        # O estado de partida tem **duas** camadas:
        #
        # 1. a arvore **acumulada** — a do commit novo anterior. E' ela
        #    que tem os ficheiros que uma fatia anterior pôs e que o
        #    pai original nao tinha.
        #
        # 2. as **remocoes** do commit original: os ficheiros que ele
        #    apaga saem, e os que ele modifica voltam a versao do pai
        #    (quando o plano nao os cobre).
        #
        # Sem a camada 2, a arvore final teria os ficheiros apagados.
        # Sem a camada 1, faltariam os ficheiros que uma fatia
        # anterior pôs.
        arvore_pai = check(git("rev-parse", parent + "^{tree}"),
                            "rev-parse").strip()
        if partida is not None and partida != arvore_pai:
            check(git_indice("read-tree", "--reset", partida), "read-tree")
        else:
            check(git_indice("read-tree", "--reset", parent), "read-tree")

        # os ficheiros que o commit apaga
        for caminho in check(git("diff", "--name-only", "--diff-filter=D",
                                 parent, commit), "diff").split():
            r = git_indice("update-index", "--force-remove", caminho)
            check(r, "update-index")

        # Os ficheiros que as fatias deste commit tocam: para esses, o
        # que fica e' o que elas puseram.
        tocados_por_fatias = set()
        for f in fatias_deste_commit:
            for entrada in f.get("files", []):
                tocados_por_fatias.add(entrada["ficheiro"] if isinstance(entrada, dict) else entrada)
            tocados_por_fatias.update(f.get("hunks", {}))
        # Os ficheiros que o commit modifica e que **nenhuma** fatia
        # toca: a modificacao nao esta no plano, e o ficheiro fica
        # como o pai.
        for caminho in check(git("diff", "--name-only", "--diff-filter=M",
                                 parent, commit), "diff").split():
            if caminho in tocados_por_fatias:
                continue
            conteudo_pai = conteudo(parent, caminho)
            if conteudo_pai is None:
                continue
            tmp = "/tmp/_split_blob"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(conteudo_pai)
            r = git_indice("hash-object", "-w", tmp)
            check(r, "hash-object")
            r = git_indice("update-index", "--add", "--cacheinfo",
                           "100644,%s,%s" % (r.stdout.strip(), caminho))
            check(r, "update-index")

    # Os ficheiros que o commit apaga, saem — em **todas** as fatias,
    # porque o `read-tree` da `partida` os repõe a cada uma.
    for caminho in check(git("diff", "--name-only", "--diff-filter=D",
                             parent, commit), "diff").split():
        r = git_indice("update-index", "--force-remove", caminho)
        check(r, "update-index")

    def por_ficheiro(caminho, funcao):
        """Aplica `funcao(base)` e devolve o texto, ou None se nao casar."""
        bases = []
        base_pai = conteudo(parent, caminho)
        acumulado = None
        if caminho in compartilhados:
            acumulado = conteudo(":" + caminho)
        if caminho in compartilhados and acumulado is not None:
            # **O acumulado primeiro.** Numa ficheiro que mais de uma
            # fatia toca, as fatias **empilham**: cada uma e' aplicada
            # sobre o resultado da anterior. E' assim que a soma da o
            # resultado original.
            if acumulado != base_pai:
                bases.append(acumulado)
        if base_pai is not None:
            bases.append(base_pai)
        if (acumulado is not None and acumulado != base_pai
                and caminho not in compartilhados):
            bases.append(acumulado)
        if not bases:
            # ficheiro **novo**: None diz ao construtor que nao ha
            # diff, e que o conteudo vem inteiro do commit
            bases.append(None)
        for b in bases:
            try:
                return funcao(b)
            except NaoCasa:
                continue
        return None

    # 1. ficheiros inteiros
    so_hunks = set(fatia.get("hunks", {}))

    for entrada in fatia.get("files", []):
        if isinstance(entrada, dict):
            caminho = entrada["ficheiro"]
            origem = entrada["de"]
        else:
            caminho = entrada
            origem = commit
        if caminho in so_hunks:
            continue
        if origem == "corrigido":
            # um ficheiro corrigido depois do commit original: nao ha
            # diff, o conteudo e' o do disco
            texto = conteudo("corrigido", caminho)
            if texto is None:
                print("ERRO: %s nao existe" % caminho, file=sys.stderr)
                sys.exit(1)
            escrever_indice(caminho, texto)
            continue
        _, hunks = dividir_hunks(diff_bruto(parent, origem, caminho))

        def constroi(construir_base, caminho=caminho, origem=origem, hunks=hunks):
            if construir_base is None:
                # ficheiro **novo**: nao ha diff para aplicar — o
                # conteudo e' o do commit, todo
                final = conteudo(origem, caminho)
                if final is None:
                    raise NaoCasa()
                return final
            # O `files` poe o conteudo **inteiro** do ficheiro, na
            # versao do commit de origem — que e' a versao final.
            if caminho not in fatia.get("hunks", {}):
                final = conteudo(origem, caminho)
                if final is None:
                    raise NaoCasa()
                return final
            raise NaoCasa()

        texto = por_ficheiro(caminho, constroi)
        if texto is None:
            print("ERRO: %s nao casou" % caminho, file=sys.stderr)
            sys.exit(1)
        escrever_indice(caminho, texto)

    # 2. ficheiros com alguns hunks
    for caminho, indices in fatia.get("hunks", {}).items():
        contra = parent
        if isinstance(indices, dict) and "contra" in indices:
            contra = indices["contra"]
            indices = indices.get("hunks", indices)
        _, hunks = dividir_hunks(diff_bruto(contra, commit, caminho))
        if isinstance(indices, dict):
            selecao = []
            for k, v in indices.items():
                i = int(k)
                if i >= len(hunks):
                    continue
                if isinstance(v, dict) and "h" in v:
                    selecao.append((i, v))
                elif isinstance(v, list) and v and isinstance(v[0], list):
                    selecao.append((i, v))
                else:
                    selecao.append((i, None))
        else:
            selecao = [(i, None) for i in indices if i < len(hunks)]
        selecao.sort()
        # cada pedaco de um hunk vira um hunk **separado**, para que
        # possa ser aplicado a um estado ja alterado
        plano_hunks = []
        for i, pedacos in selecao:
            if pedacos is None:
                plano_hunks.append((i, None))
            elif isinstance(pedacos, dict) and "h" in pedacos:
                plano_hunks.append((i, pedacos))
            else:
                for ped in pedacos:
                    plano_hunks.append((i, [ped]))

        def constroi(construir_base):
            return aplicar_hunks(construir_base, hunks, plano_hunks)

        texto = por_ficheiro(caminho, constroi)
        if texto is None:
            print("ERRO: %s nao casou" % caminho, file=sys.stderr)
            sys.exit(1)
        escrever_indice(caminho, texto)

    # 3. ficheiros com um bloco acrescentado partido
    for caminho, (ini, fim) in fatia.get("append", {}).items():
        _, hunks = dividir_hunks(diff_bruto(parent, commit, caminho))
        if len(hunks) != 1:
            print("ERRO: %s nao e' um acrescimo de um so hunk" % caminho,
                  file=sys.stderr)
            sys.exit(1)
        corpo = hunks[0]["linhas"][1:]
        novas = [l[1:] for l in corpo if l.startswith("+")]
        if any(l.startswith("-") for l in corpo):
            print("ERRO: o bloco de %s remove linhas" % caminho, file=sys.stderr)
            sys.exit(1)
        if ini < 1 or fim > len(novas):
            print("ERRO: intervalo %d..%d fora do bloco de %s (%d linhas)"
                  % (ini, fim, caminho, len(novas)), file=sys.stderr)
            sys.exit(1)
        bloco = novas[ini - 1:fim]
        # a base: o conteudo **actual** (a fatia anterior ja pode ter
        # acrescentado linhas)
        base = conteudo(":" + caminho)
        if not base.endswith("\n"):
            base += "\n"
        escrever_indice(caminho, base + "\n".join(bloco) + "\n")

    # 4. conteudo escrito a mao: substitui o ficheiro.
    #
    # Para o `CMakeLists.txt` — o unico ficheiro que muda de commit
    # para commit, e cuja lista de fontes tem de ser a de **cada**
    # fase (porque cada fase compila por si so).
    for caminho, origem in fatia.get("conteudo", {}).items():
        with open(origem, encoding="utf-8") as f:
            texto = f.read()
        escrever_indice(caminho, texto)

    # 5. linhas soltas: acrescenta uma linha depois de uma ancora.
    #
    # Para os casos em que o build precisa de uma linha que **nenhum
    # diff** introduz — a lista de fontes de um `.cpp` que o Eden
    # compila e que nos vendorizámos.
    #
    # E' uma lista de pares, porque ha casos em que sao **duas** linhas
    # a acrescentar em sitios diferentes do mesmo ficheiro.
    for pares in fatia.get("linha", []):
        for caminho, (ancora, *linhas_novas) in pares.items():
            # A base e' o conteudo **actual** (o indice): o ficheiro
            # como esta neste momento, mais a linha.
            #
            # O `read-tree` da `partida` repõe o ficheiro de um commit
            # **anterior** — e e' isso que se quer: a linha entra no
            # ficheiro como ele esta agora, e nao como o commit o tem.
            texto = conteudo(":" + caminho)
            if texto is None:
                texto = conteudo(commit, caminho)
            if texto is None:
                continue
            ls = texto.split("\n")
            pos = None
            for i, l in enumerate(ls):
                if l.strip() == ancora.strip():
                    pos = i
                    break
            if pos is None:
                print("ERRO: a ancora %r nao esta em %s" % (ancora, caminho),
                      file=sys.stderr)
                sys.exit(1)
            for k, ln in enumerate(linhas_novas):
                ls.insert(pos + 1 + k, ln)
            escrever_indice(caminho, "\n".join(ls))


def main():
    with open(sys.argv[1], encoding="utf-8") as f:
        plano = json.load(f)

    commits = check(git("rev-list", "--reverse", "HEAD"), "rev-list").split()
    print("%d commits" % len(commits))



    arvore_final_original = check(git("rev-parse", "HEAD^{tree}"), "rev-parse").strip()
    novo_head = None

    for c in commits:
        info = check(git("log", "-1", "--format=%s", c), "log").strip()
        # (o dicionario de ficheiros partilhados e' preenchido mais abaixo)
        chave = c
        for k in plano:
            if c.startswith(k):
                chave = k
                break

        tem_pai = subprocess.run(["git", "rev-parse", "--verify", c + "~1"],
                                  capture_output=True, cwd=REPO).returncode == 0
        parent = check(git("rev-parse", c + "~1"), "rev-parse").strip() \
            if tem_pai else None

        if chave not in plano:
            msg = check(git("log", "-1", "--format=%B", c), "log")
            arvore = check(git("rev-parse", c + "^{tree}"), "rev-parse").strip()
            with open("/tmp/_msg", "w", encoding="utf-8") as f:
                f.write(msg)
            args = ["commit-tree", arvore]
            # o pai e' o commit **novo** anterior, nunca o pai original:
            # e' a cadeia nova que se esta a construir
            if novo_head:
                args += ["-p", novo_head]
            r = subprocess.run(["git"] + args + ["-F", "/tmp/_msg"],
                               capture_output=True, text=True, cwd=REPO)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                sys.exit(1)
            novo_head = r.stdout.strip()
            check(git_indice("read-tree", "--reset", novo_head + "^{tree}"), "read-tree")
            continue

        fatias = plano[chave]
        print("a partir %s (%s): %d fatia(s)" % (c[:8], info, len(fatias)))

        # Os ficheiros que mais de uma fatia toca: para esses, a base
        # de cada fatia e' o estado acumulado, e nao o do pai. Sao as
        # fatias que se empilham — e o que garante que a soma da o
        # resultado original.
        contagem = {}
        for f in fatias:
            for entrada in f.get("files", []):
                caminho = entrada["ficheiro"] if isinstance(entrada, dict) else entrada
                contagem[caminho] = contagem.get(caminho, 0) + 1
            for caminho in f.get("hunks", {}):
                contagem[caminho] = contagem.get(caminho, 0) + 1
        compartilhados = {k for k, v in contagem.items() if v > 1}
        # Os ficheiros que **alguma** fatia mexe por hunks: essas nao
        # podem ser postas inteiras por outra fatia, senao o resultado
        # e' o ficheiro duas vezes.
        if compartilhados:
            print("   ficheiros partilhados: %s"
                  % ", ".join(sorted(compartilhados)))

        pai = novo_head
        # a arvore de partida: a do commit novo anterior, ou a do pai
        # original no primeiro commit
        partida = check(git_indice("rev-parse", (novo_head or "") + "^{tree}"),
                        "rev-parse").strip() if novo_head else \
                 check(git("rev-parse", parent + "^{tree}"), "rev-parse").strip()
        for n, fatia in enumerate(fatias):
            aplicar_fatia(parent, c, fatia, n == 0, compartilhados,
                          partida, fatias)
            arvore = check(git_indice("write-tree"), "write-tree").strip()
            with open("/tmp/_msg", "w", encoding="utf-8") as f:
                f.write(fatia["msg"])
                if fatia.get("body"):
                    f.write("\n\n" + fatia["body"])
            args = ["commit-tree", arvore]
            if pai:
                args += ["-p", pai]
            r = subprocess.run(["git"] + args + ["-F", "/tmp/_msg"],
                               capture_output=True, text=True, cwd=REPO)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                sys.exit(1)
            novo_head = r.stdout.strip()
            pai = novo_head
            # o indice passa a ser a arvore do commit recem-criado: e'
            # ela que a proxima fatia parte, e e' ela que guarda os
            # ficheiros que o pai original nao tem
            check(git_indice("read-tree", "--reset", novo_head + "^{tree}"),
                  "read-tree")
            print("   fatia %d: %s" % (n + 1, fatia["msg"]))

    obtido = check(git("rev-parse", novo_head + "^{tree}"), "rev-parse").strip()
    if obtido != arvore_final_original:
        # mostra o ficheiro que difere, para se poder depurar
        for caminho in check(git("diff", "--name-only", novo_head, "HEAD"),
                            "diff").split():
            print("   difere: %s" % caminho)
            d = check(git("diff", novo_head, "HEAD", "--", caminho), "diff")
            print("\n".join(d.split("\n")[:24]))
    if obtido != arvore_final_original:
        print("ERRO: a arvore final difere do HEAD original", file=sys.stderr)
        print(check(git("diff", "--stat", "HEAD", novo_head), "diff"), file=sys.stderr)
        sys.exit(1)

    # O `reset --hard` tem de operar sobre o indice **real**: o
    # `GIT_INDEX_FILE` do ambiente aponta para o temporario, e um reset
    # sobre ele deixa o repositorio inconsistente.
    env = dict(os.environ)
    env.pop("GIT_INDEX_FILE", None)
    r = subprocess.run(["git", "reset", "--hard", novo_head],
                       capture_output=True, text=True, cwd=REPO, env=env)
    if r.returncode != 0:
        print("ERRO ao mover o branch:\n%s" % r.stderr, file=sys.stderr)
        sys.exit(1)

    print("\nnovo HEAD: %s" % novo_head)
    print("arvore final identica a original")


if __name__ == "__main__":
    main()
