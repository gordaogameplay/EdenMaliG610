# Regra de publicação da `main` — EdenMaliG610

Todo e qualquer trabalho da casa passa por ramos `wip/…`. A `main` no GitHub
só representa o projecto se for **actualizada a cada avanço aceite** — não
basta fazer commit e push do ramo `wip…`: o GitHub mostra `main`. Um ramo
longe da `main` deixa o projecto com cara de "não há trabalho novo",
mesmo com tudo gravado no GitHub, atrás do ramo.

## A regra

Todas as alterações mergeadas para a `main` local têm de ser publicadas com
o avanço rápido (`--ff-only`) que falha se a `main` remota tiver andado. Em
particular:

1. Trabalhar em `wip/…` (commits por assunto, cada um com a sua medição).
2. Antes de publicar na `main`, confirmar que a série compila e o `ctest`
   passa (a regra da casa: `main` nunca fica quebrada).
3. Publicar exactamente a árvore medida: merge por avanço rápido, **nunca**
   `push --force`, nunca `reset --hard`, nunca apagar ramos.

## O bloco de comandos (usar sempre este)

Feito o trabalho em `wip/passo-X-thread` e medido, publica-se assim:

    cd /mnt/Dados/piDroid/EdenMaliG610
    git checkout main
    git merge --ff-only wip/passo-X-thread
    git push origin main
    git checkout wip/passo-X-thread

O `--ff-only` é a trava de segurança: se a `main` no GitHub tiver recebido
qualquer commit entretanto, o merge falha em vez de reescrever — nesse caso
volta-se ao `fetch`, resolve-se o estado, e só depois se publica. Em nenhuma
circunstância usar `push --force` nem `reset --hard` para "resolver".

## Diagnóstico quando algo "não aparece" no GitHub

Ordem de custo (a regra do projecto: medir, de mais barato a mais caro):

1. `git status` — há alterações sem commit?
2. `git remote -v` — o `origin` aponta para
   `git@github.com:gordaogameplay/EdenMaliG610.git`?
3. `git branch --show-current` — estou no ramo certo?
4. `git fetch origin && git rev-parse HEAD origin/main main` — local e
   remoto na mesma ponta?
5. `gh pr list -R gordaogameplay/EdenMaliG610` — há PR aberto à espera?
6. `gh api repos/gordaogameplay/EdenMaliG610/branches/main/protection` —
   a `main` está protegida? (medido a 22/10: desprotegida)

## Medição desta regra (2026-10-22)

- O ramo `origin/wip/passo-4-thread` estava no mesmo hash local
  (`a32f268…`), a `origin/main` parada em `8c27236` (20/10): o trabalho
  estava gravado mas **invisível** na vista do GitHub, que mostra `main`.
- A ponta da `main` é ancestral directa da wip (merge-base = ponta da
  `main`, `0 ↔ 105`): o avanço rápido existe e é seguro.
- Corrigido: merges rápidos para a `main` a partir de agora, no bloco de
  cima.
