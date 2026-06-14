/* ==========================================================================
 *  TRABALHO DO GRAU B - FRACTAL DE MANDELBROT EM PARALELO (Pthreads + SDL2)
 *  
 *  Integrantes: Cícero Calsing, Clara Burghardt, Patrícia Nagel 
 * 
 *  Arquitetura:
 *    - thread MAIN .......... cria as tarefas (quadrados) e enfileira no buffer
 *    - threads TRABALHADORAS  pegam tarefa -> computam -> gravam resultado
 *    - thread de PRINT ...... lê os resultados e desenha na tela "on the fly"
 *
 *  Dois buffers compartilhados, cada um com seu jogo produtor/consumidor:
 *    1) buffer de TAREFAS    : main = produtor   , trabalhadoras = consumidoras
 *    2) buffer de RESULTADOS : trabalhadoras = produtoras , print = consumidora
 *
 *  Compilar:  gcc mandelbrot.c -o mandelbrot -lSDL2 -lpthread -lm 
 *  Rodar:     ./mandelbrot [num_threads] [max_iter] [tam_tarefa]
 *  Ex.:       ./mandelbrot 8 1500 24
 * ========================================================================== */

#include <SDL2/SDL.h>   // biblioteca grafica: janela, textura, desenho na tela
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

/* ----------------------- Configuracao da imagem/janela ------------------- */
#define LARGURA 900     // largura da janela em pixels
#define ALTURA  600     // altura  da janela em pixels

/* Regiao do plano complexo que vamos enquadrar (vista classica do Mandelbrot) */
#define REAL_MIN (-2.5) // parte real minima
#define REAL_MAX ( 1.0) // parte real maxima

/* Capacidade (tamanho) dos buffers circulares compartilhados */
#define CAP_TAREFAS    512  // quantas tarefas cabem no buffer de tarefas
#define CAP_RESULTADOS 512  // quantas tarefas prontas cabem no buffer de resultados

/* ----------------------- Parametros do programa -------------------------- */
/* Sao "globais" para todas as threads enxergarem; definidos pela linha de comando */
int num_threads = 4;     // numero de threads trabalhadoras (parametro 1)
int max_iter    = 1000;  // complexidade do Mandelbrot = nº de iteracoes (parametro 2)
int tam_tarefa  = 32;    // lado de cada quadrado/tarefa, em pixels (parametro 3)

double imag_min, imag_max; // limites do eixo imaginario (calculados p/ nao distorcer)

/* ----------------------- Estrutura de uma TAREFA ------------------------- */
/* Uma tarefa é um quadrado da imagem: cantos (x,y) inicial e final em pixels */
typedef struct {
    int x_ini, y_ini;   // canto superior esquerdo do quadrado
    int x_fim, y_fim;   // canto inferior direito (exclusivo) do quadrado
} Tarefa;

/* ----------------------- Framebuffer compartilhado ----------------------- */
/* Vetor de cores (1 Uint32 = 1 pixel ARGB). Cada trabalhadora escreve apenas */
/* nos pixels do seu quadrado, entao nao ha duas threads no mesmo pixel.      */
Uint32 *framebuffer; // alocado no main com tamanho LARGURA*ALTURA

/* ===================== BUFFER 1: FILA DE TAREFAS ========================== */
Tarefa fila_tarefas[CAP_TAREFAS];        // armazenamento circular das tarefas
int t_inicio = 0;                        // indice de onde vamos RETIRAR (consumir)
int t_fim    = 0;                        // indice de onde vamos INSERIR (produzir)
int t_qtd    = 0;                        // quantas tarefas estao no buffer agora
bool producao_encerrada = false;         // true quando o main ja criou TODAS as tarefas

pthread_mutex_t mtx_tarefas;             // cadeado (lock/unlock) que protege a fila de tarefas
pthread_cond_t  cond_tarefa_disponivel;  // condicao "tem tarefa para consumir" (buffer nao-vazio)
pthread_cond_t  cond_espaco_tarefa;      // condicao "tem espaco para produzir" (buffer nao-cheio)

/* ===================== BUFFER 2: FILA DE RESULTADOS ======================= */
Tarefa fila_resultados[CAP_RESULTADOS];      // quadrados ja calculados, prontos p/ desenhar
int r_inicio = 0;                            // indice de retirada (a thread de print consome)
int r_fim    = 0;                            // indice de insercao (as trabalhadoras produzem)
int r_qtd    = 0;                            // quantos resultados estao no buffer agora

pthread_mutex_t mtx_resultados;              // cadeado que protege a fila de resultados
pthread_cond_t  cond_resultado_disponivel;   // condicao "tem resultado para desenhar"
pthread_cond_t  cond_espaco_resultado;       // condicao "tem espaco para gravar resultado"

/* ===================== FUNCOES DO BUFFER DE TAREFAS ====================== */

/* Produtor (main) coloca uma tarefa no buffer; bloqueia se o buffer estiver cheio */
void enfileirar_tarefa(Tarefa t) {
    pthread_mutex_lock(&mtx_tarefas);                         // tranca: só eu mexo na fila agora
    while (t_qtd == CAP_TAREFAS)                              // enquanto a fila estiver cheia...
        pthread_cond_wait(&cond_espaco_tarefa, &mtx_tarefas); // ...durmo ate liberar espaco
    fila_tarefas[t_fim] = t;                                  // gravo a tarefa na posicao de insercao
    t_fim = (t_fim + 1) % CAP_TAREFAS;                        // avanco o indice de insercao (circular)
    t_qtd++;                                                  // tenho uma tarefa a mais no buffer
    pthread_cond_signal(&cond_tarefa_disponivel);             // acordo UMA trabalhadora que esperava tarefa
    pthread_mutex_unlock(&mtx_tarefas);                       // destranco: outras threads podem mexer
}

/* Consumidor (trabalhadora) retira uma tarefa; retorna false quando acabou tudo */
bool desenfileirar_tarefa(Tarefa *out) {
    pthread_mutex_lock(&mtx_tarefas);                             // tranca o acesso a fila
    while (t_qtd == 0 && !producao_encerrada)                     // enquanto vazia E o main ainda produz...
        pthread_cond_wait(&cond_tarefa_disponivel, &mtx_tarefas); // ...durmo esperando tarefa chegar
    if (t_qtd == 0 && producao_encerrada) {                       // se esvaziou E nao vem mais nada...
        pthread_mutex_unlock(&mtx_tarefas);                       // ...destranco
        return false;                                             // ...e aviso a trabalhadora para encerrar
    }
    *out = fila_tarefas[t_inicio];                          // copio a tarefa da posicao de retirada
    t_inicio = (t_inicio + 1) % CAP_TAREFAS;                // avanco o indice de retirada (circular)
    t_qtd--;                                                // tenho uma tarefa a menos no buffer
    pthread_cond_signal(&cond_espaco_tarefa);               // acordo o produtor que esperava espaco
    pthread_mutex_unlock(&mtx_tarefas);                     // destranco
    return true;                                            // peguei uma tarefa com sucesso
}

/* ===================== FUNCOES DO BUFFER DE RESULTADOS =================== */

/* Produtor (trabalhadora) avisa que um quadrado ficou pronto para ser desenhado */
void enfileirar_resultado(Tarefa t) {
    pthread_mutex_lock(&mtx_resultados);                            // tranca a fila de resultados
    while (r_qtd == CAP_RESULTADOS)                                 // enquanto cheia...
        pthread_cond_wait(&cond_espaco_resultado, &mtx_resultados); // ...espero a thread de print consumir
    fila_resultados[r_fim] = t;                                     // gravo a tarefa pronta
    r_fim = (r_fim + 1) % CAP_RESULTADOS;                           // avanco indice de insercao (circular)
    r_qtd++;                                                        // mais um resultado disponivel
    pthread_cond_signal(&cond_resultado_disponivel);                // acordo a thread de print
    pthread_mutex_unlock(&mtx_resultados);                          // destranco
}

/* Consumidor (print): tenta pegar um resultado SEM bloquear, p/ a janela seguir respondendo */
bool tentar_desenfileirar_resultado(Tarefa *out) {
    pthread_mutex_lock(&mtx_resultados);                        // tranca a fila de resultados
    if (r_qtd == 0) {                                           // se nao ha nada pronto agora...
        pthread_mutex_unlock(&mtx_resultados);                  // ...destranco
        return false;                                           // ...e retorno sem esperar
    }
    *out = fila_resultados[r_inicio];                           // copio o resultado da retirada
    r_inicio = (r_inicio + 1) % CAP_RESULTADOS;                 // avanco indice de retirada (circular)
    r_qtd--;                                                    // um resultado a menos no buffer
    pthread_cond_signal(&cond_espaco_resultado);                // acordo trabalhadora que esperava espaco
    pthread_mutex_unlock(&mtx_resultados);                      // destranco
    return true;                                                // peguei um resultado pronto
}

/* ===================== MATEMATICA DO MANDELBROT ========================== */

/* Calcula quantas iteracoes o ponto (px,py) leva para "escapar"; isso vira a cor */
int mandelbrot(int px, int py) {
    /* Converte a coordenada de PIXEL para a coordenada no PLANO COMPLEXO */
    double c_re = REAL_MIN + (double)px / (LARGURA - 1) * (REAL_MAX - REAL_MIN); // parte real de c
    double c_im = imag_min + (double)py / (ALTURA  - 1) * (imag_max - imag_min); // parte imaginaria de c
    double z_re = 0.0, z_im = 0.0;        // z comeca em 0 (z0 = 0)
    int iter = 0;                         // contador de iteracoes
    /* Itera z = z^2 + c ate o modulo passar de 2 (|z|^2 > 4) ou bater o limite */
    while (z_re * z_re + z_im * z_im <= 4.0 && iter < max_iter) {
        double novo_re = z_re * z_re - z_im * z_im + c_re; // parte real de z^2 + c
        double novo_im = 2.0 * z_re * z_im + c_im;         // parte imaginaria de z^2 + c
        z_re = novo_re;                   // atualizo a parte real de z
        z_im = novo_im;                   // atualizo a parte imaginaria de z
        iter++;                           // contei mais uma iteracao
    }
    return iter;                          // devolvo o numero de iteracoes ate escapar
}

/* Transforma o numero de iteracoes em uma cor ARGB bonita usando escala logaritmica */
Uint32 cor_do_iter(int iter) {
    if (iter == max_iter) return 0xFF000000; // dentro do conjunto = preto

    /* log suaviza a distribuicao de cores: pixels proximos ao conjunto ganham mais variacao */
    double log_iter = log(iter + 1.0) / log(max_iter + 1.0);

    /* tres senos defasados em 2.1 radianos geram um gradiente colorido ciclico */
    Uint8 r = (Uint8)(sin(log_iter * 3.14159 * 6.0 + 0.0) * 127 + 128); // componente vermelho
    Uint8 g = (Uint8)(sin(log_iter * 3.14159 * 6.0 + 2.1) * 127 + 128); // componente verde
    Uint8 b = (Uint8)(sin(log_iter * 3.14159 * 6.0 + 4.2) * 127 + 128); // componente azul

    return (0xFFu << 24) | (r << 16) | (g << 8) | b; // monta o pixel ARGB (alpha sempre 255)
}

/* ===================== THREAD TRABALHADORA =============================== */

/* Cada trabalhadora repete: pega tarefa -> computa o quadrado -> grava resultado */
void *funcao_trabalhadora(void *arg) {
    (void)arg;                            // nao usamos o argumento; isso evita warning
    Tarefa t;                             // aqui guardo a tarefa que eu pegar
    while (desenfileirar_tarefa(&t)) {    // enquanto houver tarefa para consumir...
        /* Computa o Mandelbrot de cada pixel DENTRO do quadrado desta tarefa */
        for (int py = t.y_ini; py < t.y_fim; py++)              // percorre as linhas do quadrado
            for (int px = t.x_ini; px < t.x_fim; px++) {        // percorre as colunas do quadrado
                int iter = mandelbrot(px, py);                  // calcula iteracoes daquele pixel
                framebuffer[py * LARGURA + px] = cor_do_iter(iter); // grava a cor no framebuffer
            }
        enfileirar_resultado(t);          // avisa a thread de print que este quadrado ficou pronto
    }
    return NULL;                          // sem mais tarefas -> a trabalhadora termina
}

/* ===================== THREAD DE PRINT (desenho na tela) ================= */

int total_tarefas; // total de quadrados (calculado no main) - a print sabe quando acabou

/* A thread de print cuida de TODO o SDL: janela, textura e desenho dinamico   */
void *funcao_print(void *arg) {
    (void)arg;                                                  // argumento nao utilizado

    SDL_Init(SDL_INIT_VIDEO);                                   // inicializa o subsistema de video do SDL
    SDL_Window *janela = SDL_CreateWindow(                      // cria a janela que "salta" na tela
        "Mandelbrot Paralelo",                                  // titulo da janela
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,         // posicao centralizada na tela
        LARGURA, ALTURA, SDL_WINDOW_SHOWN);                     // dimensoes e janela visivel
    SDL_Renderer *renderer = SDL_CreateRenderer(                // cria o "renderizador" que desenha na janela
        janela, -1, SDL_RENDERER_ACCELERATED);                  // -1 = primeiro driver disponivel, com GPU
    SDL_Texture *textura = SDL_CreateTexture(                   // cria a textura (imagem) que vamos atualizar
        renderer, SDL_PIXELFORMAT_ARGB8888,                     // formato de cor igual ao do framebuffer
        SDL_TEXTUREACCESS_STREAMING, LARGURA, ALTURA);          // STREAMING = atualizada com frequencia

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // preto
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    bool rodando = true;                                        // controla o laco principal de desenho
    int recebidos = 0;                                          // quantos quadrados ja desenhei
    SDL_Event evento;                                           // estrutura para ler eventos da janela
    Tarefa t;                                                   // quadrado pronto que vou desenhar

    while (rodando) {                                           // laco principal da thread de print
        /* 1) Trata eventos da janela (ex.: clicar no X para fechar) */
        while (SDL_PollEvent(&evento))                          // pega cada evento pendente
            if (evento.type == SDL_QUIT)                        // se o usuario fechou a janela...
                rodando = false;                                // ...sinaliza para sair do laco

        /* 2) Consome TODOS os resultados disponiveis agora (sem travar a janela) */
        while (tentar_desenfileirar_resultado(&t)) {            // enquanto houver quadrado pronto...
            SDL_Rect area = {                                   // define o retangulo (sub-area) a atualizar
                t.x_ini, t.y_ini,                               // canto superior esquerdo
                t.x_fim - t.x_ini, t.y_fim - t.y_ini };         // largura e altura do quadrado
            SDL_UpdateTexture(                                  // copia so aquele quadrado p/ a textura
                textura, &area,                                 // textura e a area a atualizar
                &framebuffer[t.y_ini * LARGURA + t.x_ini],      // ponteiro p/ o 1o pixel do quadrado
                LARGURA * sizeof(Uint32));                      // "pitch": bytes por linha do framebuffer
            recebidos++;                                        // contei mais um quadrado desenhado
        }

        /* 3) Joga a textura na tela -> e aqui que o desenho aparece "on the fly" */
        SDL_RenderClear(renderer);                              // limpa o quadro atual
        SDL_RenderCopy(renderer, textura, NULL, NULL);          // copia a textura inteira para a janela
        SDL_RenderPresent(renderer);                            // mostra o resultado na tela
        SDL_Delay(16);                                          // pausa ~16ms (~60 quadros por segundo)
    }

    SDL_DestroyTexture(textura);                                // libera a textura
    SDL_DestroyRenderer(renderer);                              // libera o renderizador
    SDL_DestroyWindow(janela);                                  // fecha a janela
    SDL_Quit();                                                 // encerra o SDL
    return NULL;                                                // thread de print termina
}

/* ============================== MAIN ==================================== */

int main(int argc, char *argv[]) {
    /* Le os parametros da linha de comando, se forem informados */
    if (argc > 1) num_threads = atoi(argv[1]);                  // parametro 1: nº de threads
    if (argc > 2) max_iter    = atoi(argv[2]);                  // parametro 2: complexidade (iteracoes)
    if (argc > 3) tam_tarefa  = atoi(argv[3]);                  // parametro 3: tamanho do quadrado

    /* Ajusta o eixo imaginario para a imagem NAO ficar distorcida (mantem proporcao) */
    double meio_imag = (REAL_MAX - REAL_MIN) * (double)ALTURA / LARGURA / 2.0; // metade da altura no plano
    imag_min = -meio_imag;                                      // limite inferior do eixo imaginario
    imag_max =  meio_imag;                                      // limite superior do eixo imaginario

    /* Aloca o framebuffer compartilhado (uma cor por pixel da imagem) */
    framebuffer = malloc((size_t)LARGURA * ALTURA * sizeof(Uint32)); // reserva a memoria
    for (int i = 0; i < LARGURA * ALTURA; i++)                  // percorre todos os pixels
        framebuffer[i] = 0xFF000000;                            // inicia tudo preto (fundo)

    /* Inicializa os mutexes e variaveis de condicao dos dois buffers */
    pthread_mutex_init(&mtx_tarefas, NULL);                     // cadeado do buffer de tarefas
    pthread_cond_init(&cond_tarefa_disponivel, NULL);           // condicao "tem tarefa"
    pthread_cond_init(&cond_espaco_tarefa, NULL);               // condicao "tem espaco p/ tarefa"
    pthread_mutex_init(&mtx_resultados, NULL);                  // cadeado do buffer de resultados
    pthread_cond_init(&cond_resultado_disponivel, NULL);        // condicao "tem resultado"
    pthread_cond_init(&cond_espaco_resultado, NULL);            // condicao "tem espaco p/ resultado"

    /* Calcula quantos quadrados (tarefas) a imagem tera no total */
    int nx = (LARGURA + tam_tarefa - 1) / tam_tarefa;           // nº de quadrados na horizontal
    int ny = (ALTURA  + tam_tarefa - 1) / tam_tarefa;           // nº de quadrados na vertical
    total_tarefas = nx * ny;                                    // total de quadrados a computar

    /* Cria a thread de PRINT (ela liga o SDL e mostra a janela) */
    pthread_t thread_print;                                     // identificador da thread de print
    pthread_create(&thread_print, NULL, funcao_print, NULL);    // dispara a thread de print

    /* Cria as N threads TRABALHADORAS */
    pthread_t *trabalhadoras = malloc(num_threads * sizeof(pthread_t)); // vetor de IDs das threads
    for (int i = 0; i < num_threads; i++)                               // para cada trabalhadora...
        pthread_create(&trabalhadoras[i], NULL, funcao_trabalhadora, NULL); // ...dispara a thread

    /* A thread MAIN agora PRODUZ as tarefas: divide a imagem em QUADRADOS */
    for (int y = 0; y < ALTURA; y += tam_tarefa)                // percorre a imagem em faixas de altura
        for (int x = 0; x < LARGURA; x += tam_tarefa) {         // percorre a imagem em faixas de largura
            Tarefa t;                                           // monta uma tarefa (um quadrado)
            t.x_ini = x;                                        // canto x inicial do quadrado
            t.y_ini = y;                                        // canto y inicial do quadrado
            t.x_fim = (x + tam_tarefa < LARGURA) ? x + tam_tarefa : LARGURA; // x final (sem passar da borda)
            t.y_fim = (y + tam_tarefa < ALTURA)  ? y + tam_tarefa : ALTURA;  // y final (sem passar da borda)
            enfileirar_tarefa(t);                               // coloca o quadrado no buffer de tarefas
        }

    /* Avisa as trabalhadoras que NAO existem mais tarefas a produzir */
    pthread_mutex_lock(&mtx_tarefas);                           // tranca antes de mudar a flag
    producao_encerrada = true;                                  // marca o fim da producao de tarefas
    pthread_cond_broadcast(&cond_tarefa_disponivel);            // acorda TODAS as trabalhadoras paradas
    pthread_mutex_unlock(&mtx_tarefas);                         // destranca

    /* Espera todas as trabalhadoras terminarem seus quadrados */
    for (int i = 0; i < num_threads; i++)                       // para cada trabalhadora...
        pthread_join(trabalhadoras[i], NULL);                   // ...aguarda ela encerrar

    /* Espera o usuario fechar a janela (a thread de print so sai no SDL_QUIT) */
    pthread_join(thread_print, NULL);                           // aguarda a thread de print terminar

    /* Libera toda a memoria e os recursos de sincronizacao */
    free(trabalhadoras);                                        // libera o vetor de IDs
    free(framebuffer);                                          // libera o framebuffer
    pthread_mutex_destroy(&mtx_tarefas);                        // destroi o mutex de tarefas
    pthread_mutex_destroy(&mtx_resultados);                     // destroi o mutex de resultados
    pthread_cond_destroy(&cond_tarefa_disponivel);              // destroi as variaveis de condicao...
    pthread_cond_destroy(&cond_espaco_tarefa);
    pthread_cond_destroy(&cond_resultado_disponivel);
    pthread_cond_destroy(&cond_espaco_resultado);
    return 0;                                                   // fim do programa
}
