# Modelagem do Programa — Mandelbrot Paralelo

## Integrantes: 
- Cícero Calsing
- Clara Burghardt
- Patrícia Nagel

## Visão Geral da Arquitetura

O programa é dividido em **três atores** que rodam em paralelo, conectados por **dois buffers circulares**:

```
┌─────────────────────────────────────────────────────────────────┐
│                         PROGRAMA                                │
│                                                                 │
│   ┌──────────┐     ┌─────────────────┐     ┌────────────────┐  │
│   │  Thread  │     │    Threads      │     │    Thread      │  │
│   │   MAIN   │     │  TRABALHADORAS  │     │    PRINT       │  │
│   │          │     │   (N threads)   │     │                │  │
│   │ Divide a │     │                 │     │ Inicializa SDL │  │
│   │ imagem   │     │ Pega tarefa     │     │ Abre a janela  │  │
│   │ em       │     │ Calcula pixels  │     │ Desenha blocos │  │
│   │ quadrados│     │ Grava cores     │     │ na tela        │  │
│   │          │     │ Avisa resultado │     │ ~60fps         │  │
│   └────┬─────┘     └───────┬─────────┘     └───────▲────────┘  │
│        │                   │                        │           │
│        │  [BUFFER 1]       │          [BUFFER 2]    │           │
│        │  Fila de Tarefas  │          Fila de       │           │
│        └──────────────────►│          Resultados    │           │
│                            └───────────────────────►│           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Os Três Atores

### Thread MAIN (Produtor de Tarefas)
- Lê os parâmetros da linha de comando
- Aloca o framebuffer
- Inicializa mutexes e variáveis de condição
- Dispara todas as threads
- **Divide a janela em quadradinhos** e enfileira no Buffer 1
- Ao terminar, sinaliza `producao_encerrada = true` e faz `broadcast`
- Aguarda todas as threads terminarem com `pthread_join`

### Threads TRABALHADORAS (Consumidoras/Produtoras)
- Ficam em loop: **pega tarefa → calcula → grava resultado**
- Para cada pixel do quadradinho, chama `mandelbrot(px, py)`
- Grava a cor calculada diretamente no **framebuffer**
- Enfileira o quadradinho no Buffer 2 avisando que está pronto
- Terminam quando não há mais tarefas e a produção está encerrada

### Thread PRINT (Consumidora de Resultados)
- Inicializa a janela SDL
- Fica num loop de ~60fps
- Tenta consumir resultados do Buffer 2 **sem bloquear**
- Para cada resultado, atualiza **só aquela região** da textura SDL
- Renderiza a textura na tela
- Termina quando o usuário fecha a janela

---

## Os Dois Buffers Circulares

```
BUFFER 1 — Fila de Tarefas (capacidade: 512)
┌────┬────┬────┬────┬────┬────┬────┬────┐
│ T0 │ T1 │ T2 │ T3 │    │    │    │    │
└────┴────┴────┴────┴────┴────┴────┴────┘
       ▲                 ▲
   t_inicio           t_fim
   (trabalhadora       (main
    consome aqui)       insere aqui)

BUFFER 2 — Fila de Resultados (capacidade: 512)
┌────┬────┬────┬────┬────┬────┬────┬────┐
│ R0 │ R1 │    │    │    │    │    │    │
└────┴────┴────┴────┴────┴────┴────┴────┘
       ▲         ▲
   r_inicio   r_fim
   (print      (trabalhadora
    consome)    insere)
```

O índice avança com `%` para voltar ao começo automaticamente:
```c
t_fim = (t_fim + 1) % CAP_TAREFAS;  // circular: 511 → 0
```

---

## O Framebuffer

```
Janela 900 x 600 pixels = 540.000 pixels

framebuffer[0]          → pixel (0, 0)    canto superior esquerdo
framebuffer[1]          → pixel (1, 0)
...
framebuffer[900]        → pixel (0, 1)    início da segunda linha
...
framebuffer[539.999]    → pixel (899, 599) canto inferior direito

Fórmula: framebuffer[py * 900 + px]
```

Cada thread trabalhadora escreve **apenas nos pixels do seu quadradinho**, sem conflito com as outras — por isso o framebuffer **não precisa de mutex**.

---

## Sincronização (Mutex + Variável de Condição)

Cada buffer tem **1 mutex** e **2 variáveis de condição**:

```
Buffer de Tarefas:
  mtx_tarefas              → protege acesso à fila
  cond_tarefa_disponivel   → acorda trabalhadora quando chega tarefa
  cond_espaco_tarefa       → acorda main quando libera espaço

Buffer de Resultados:
  mtx_resultados           → protege acesso à fila
  cond_resultado_disponivel → acorda print quando chega resultado
  cond_espaco_resultado    → acorda trabalhadora quando libera espaço
```

### Fluxo do mutex na prática

```
MAIN enfileirando tarefa:
  lock(mutex)
    enquanto fila cheia → dorme (libera mutex automaticamente)
    grava tarefa
    signal → acorda 1 trabalhadora
  unlock(mutex)

TRABALHADORA desenfileirando tarefa:
  lock(mutex)
    enquanto fila vazia E produção não encerrada → dorme
    se vazia E encerrada → unlock + return false (encerra)
    pega tarefa
    signal → acorda main se ele estava esperando espaço
  unlock(mutex)
```

### Por que `while` e não `if` no wait?

```c
// ERRADO — vulnerável a spurious wakeup
if (t_qtd == 0)
    pthread_cond_wait(...);

// CORRETO — verifica de novo ao acordar
while (t_qtd == 0 && !producao_encerrada)
    pthread_cond_wait(...);
```

Uma thread pode acordar sem que a condição seja verdadeira (spurious wakeup). O `while` garante que ela verifique de novo antes de prosseguir.

### Por que `broadcast` no final e não `signal`?

```c
pthread_cond_broadcast(&cond_tarefa_disponivel);
```

`signal` acorda apenas **uma** thread. Se houver 8 trabalhadoras dormindo esperando tarefa, precisamos acordar **todas** de uma vez para que todas percebam que a produção encerrou e possam terminar. Com `signal`, 7 ficariam dormindo para sempre.

---

## Fluxo Completo de um Quadradinho

```
1. MAIN calcula os cantos do quadradinho (x_ini, y_ini, x_fim, y_fim)
        ↓
2. MAIN chama enfileirar_tarefa() → quadradinho entra no Buffer 1
        ↓
3. TRABALHADORA acorda, pega o quadradinho do Buffer 1
        ↓
4. TRABALHADORA percorre cada pixel (px, py) do quadradinho
        ↓
5. Para cada pixel: iter = mandelbrot(px, py)
        ↓
6. cor = cor_do_iter(iter) → grava em framebuffer[py * 900 + px]
        ↓
7. TRABALHADORA chama enfileirar_resultado() → quadradinho entra no Buffer 2
        ↓
8. PRINT acorda, pega o quadradinho do Buffer 2
        ↓
9. PRINT chama SDL_UpdateTexture() apenas na região daquele quadradinho
        ↓
10. PRINT chama SDL_RenderPresent() → quadradinho aparece na tela
```

---

## Parâmetros e seu Efeito

| Parâmetro | O que controla | Efeito visual |
|-----------|---------------|---------------|
| `num_threads` | Quantos núcleos trabalham em paralelo | Mais threads = imagem aparece mais rápido |
| `max_iter` | Detalhe e variação de cor do fractal | Maior = mais detalhe nas bordas, mais pesado |
| `tam_tarefa` | Tamanho em pixels de cada quadradinho | Menor = mais blocos, paralelismo mais visível |

Exemplo recomendado para ver o paralelismo:
```bash
./mandelbrot 4 10000 16
```
