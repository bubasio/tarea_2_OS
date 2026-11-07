#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <signal.h>

// config que sale del txt
typedef struct {
    int seed;
    int delay_min;
    int delay_max;
    int puma_init_x;
    int puma_init_y;
} config_juego;

// corte del servidor y cupo de mesas
volatile sig_atomic_t servidor_activo = 1; 
sem_t mesas_sem; 

// si no hay config.txt
void inicializar_config_default(config_juego *c) {
    c->seed = 12345;
    c->delay_min = 50;
    c->delay_max = 300;
    c->puma_init_x = 2; 
    c->puma_init_y = 2;
}

// lee config si viene
void leer_config(const char *filename, config_juego *c) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("[config] No se pudo abrir el archivo, usando valores por defecto");
        return;
    }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), file)) {
        if (strncmp(buffer, "CONFIG", 6) == 0) {
            sscanf(buffer, "CONFIG SEED %d DELAY_MIN %d DELAY_MAX %d PUMA_INIT %d %d",
                   &c->seed, &c->delay_min, &c->delay_max,
                   &c->puma_init_x, &c->puma_init_y);
        }
    }
    fclose(file);
}

// 25 del tablero + 3 del trono
#define num_casillas 28 

typedef enum { vacia, puma, perro } tipo_ficha;

typedef struct {
    tipo_ficha ocupante;
    int id_perro; 
    pthread_mutex_t mutex_casilla;
} casilla;

typedef struct {
    casilla tablero[num_casillas];
    int capturas_puma;
    int perros_vivos;
    int partida_activa;
    int puma_conectado;
    int perros_conectados;
    pthread_mutex_t mutex_estado;
} estado_mesa;

// puma arriba, perros abajo
void colocar_fichas_iniciales(estado_mesa *mesa, config_juego *config) {
    int pos_puma = (config->puma_init_y * 5) + config->puma_init_x;
    mesa->tablero[pos_puma].ocupante = puma;
    
    int perros_colocados = 0;
    for (int i = 24; i >= 0 && perros_colocados < 12; i--) {
        if (mesa->tablero[i].ocupante == vacia) {
            mesa->tablero[i].ocupante = perro;
            mesa->tablero[i].id_perro = perros_colocados;
            perros_colocados++;
        }
    }
}

// vecinas en el 5x5
int es_adyacente(int origen, int destino) {
    if (origen < 0 || origen >= num_casillas || destino < 0 || destino >= num_casillas) {
        return 0;
    }
    if (origen < 25 && destino < 25) {
        int x1 = origen % 5;
        int y1 = origen / 5;
        int x2 = destino % 5;
        int y2 = destino / 5;
        int dx = abs(x1 - x2);
        int dy = abs(y1 - y2);
        if (dx <= 1 && dy <= 1 && (dx != 0 || dy != 0)) {
            return 1;
        }
    }
    return 0; 
}

void inicializar_tablero(estado_mesa *mesa) {
    mesa->capturas_puma = 0;
    mesa->perros_vivos = 12;
    mesa->partida_activa = 1;
    mesa->puma_conectado = 1;
    mesa->perros_conectados = 12;
    pthread_mutex_init(&mesa->mutex_estado, NULL);

    for (int i = 0; i < num_casillas; i++) {
        mesa->tablero[i].ocupante = vacia;
        mesa->tablero[i].id_perro = -1;
        pthread_mutex_init(&mesa->tablero[i].mutex_casilla, NULL); 
    }
}

// orden fijo para no deadlock
void bloquear_casillas(estado_mesa *mesa, int c1, int c2) {
    int primero = (c1 < c2) ? c1 : c2;
    int segundo = (c1 < c2) ? c2 : c1;
    pthread_mutex_lock(&mesa->tablero[primero].mutex_casilla);
    pthread_mutex_lock(&mesa->tablero[segundo].mutex_casilla);
}

void desbloquear_casillas(estado_mesa *mesa, int c1, int c2) {
    int primero = (c1 < c2) ? c1 : c2;
    int segundo = (c1 < c2) ? c2 : c1;
    pthread_mutex_unlock(&mesa->tablero[segundo].mutex_casilla);
    pthread_mutex_unlock(&mesa->tablero[primero].mutex_casilla);
}

// casilla saltada o -1
int obtener_casilla_intermedia(int origen, int destino) {
    if (origen >= 25 || destino >= 25) return -1; 
    int x1 = origen % 5, y1 = origen / 5;
    int x2 = destino % 5, y2 = destino / 5;
    
    if (abs(x1 - x2) == 2 && abs(y1 - y2) == 0) return origen + (x2 - x1) / 2; 
    if (abs(x1 - x2) == 0 && abs(y1 - y2) == 2) return origen + ((y2 - y1) / 2) * 5; 
    if (abs(x1 - x2) == 2 && abs(y1 - y2) == 2) return origen + (x2 - x1) / 2 + ((y2 - y1) / 2) * 5; 
    
    return -1;
}

// mueve con mutex
int intentar_movimiento(estado_mesa *mesa, int origen, int destino, tipo_ficha tipo, int id_perro) {
    int exito = 0;
    int casilla_intermedia = (tipo == puma) ? obtener_casilla_intermedia(origen, destino) : -1;

    if (casilla_intermedia != -1) {
        int casillas[3] = {origen, destino, casilla_intermedia};
        for (int i = 0; i < 2; i++) {
            for (int j = i + 1; j < 3; j++) {
                if (casillas[i] > casillas[j]) { 
                    int temp = casillas[i]; 
                    casillas[i] = casillas[j]; 
                    casillas[j] = temp; 
                }
            }
        }
        pthread_mutex_lock(&mesa->tablero[casillas[0]].mutex_casilla);
        pthread_mutex_lock(&mesa->tablero[casillas[1]].mutex_casilla);
        pthread_mutex_lock(&mesa->tablero[casillas[2]].mutex_casilla);
        pthread_mutex_lock(&mesa->mutex_estado); 

        if (mesa->tablero[destino].ocupante == vacia && 
            mesa->tablero[origen].ocupante == puma && 
            mesa->tablero[casilla_intermedia].ocupante == perro) {
            
            mesa->tablero[destino].ocupante = puma;
            mesa->tablero[origen].ocupante = vacia;
            mesa->tablero[casilla_intermedia].ocupante = vacia;
            mesa->capturas_puma++;
            mesa->perros_vivos--;
            exito = 1;
        }

        pthread_mutex_unlock(&mesa->mutex_estado);
        pthread_mutex_unlock(&mesa->tablero[casillas[2]].mutex_casilla);
        pthread_mutex_unlock(&mesa->tablero[casillas[1]].mutex_casilla);
        pthread_mutex_unlock(&mesa->tablero[casillas[0]].mutex_casilla);
    } else {
        bloquear_casillas(mesa, origen, destino);
        if (mesa->tablero[destino].ocupante == vacia && mesa->tablero[origen].ocupante == tipo) {
            mesa->tablero[destino].ocupante = tipo;
            mesa->tablero[destino].id_perro = id_perro;
            mesa->tablero[origen].ocupante = vacia;
            mesa->tablero[origen].id_perro = -1;
            exito = 1;
        }
        desbloquear_casillas(mesa, origen, destino);
    }
    return exito;
}

typedef struct {
    int id_mesa;
    config_juego config;
} argumentos_mesa;

typedef struct {
    int id; 
    estado_mesa *mesa;
    config_juego *config;
} argumentos_ficha;

void destruir_tablero(estado_mesa *mesa);

// espera random
void retardo_aleatorio(config_juego *config) {
    int retardo_ms = config->delay_min + rand() % (config->delay_max - config->delay_min + 1);
    usleep(retardo_ms * 1000); 
}

// hay movida?
int puma_tiene_movimientos(estado_mesa *mesa, int pos_puma) {
    for (int destino = 0; destino < num_casillas; destino++) {
        if (mesa->tablero[destino].ocupante == vacia) {
            if (es_adyacente(pos_puma, destino)) return 1; 
            if (obtener_casilla_intermedia(pos_puma, destino) != -1 && 
                mesa->tablero[obtener_casilla_intermedia(pos_puma, destino)].ocupante == perro) {
                return 1; 
            }
        }
    }
    return 0;
}

void* hilo_puma(void* arg) {
    argumentos_ficha *args = (argumentos_ficha*)arg;
    
    while (args->mesa->partida_activa && servidor_activo) {
        retardo_aleatorio(args->config);
        
        int pos_actual = -1;
        for (int i = 0; i < num_casillas; i++) {
            if (args->mesa->tablero[i].ocupante == puma) {
                pos_actual = i;
                break;
            }
        }
        
        if (pos_actual != -1) {
            // sin movidas, pierde
            if (!puma_tiene_movimientos(args->mesa, pos_actual)) {
                args->mesa->partida_activa = 0;
                break;
            }

            int destino_intento = rand() % num_casillas;
            if (es_adyacente(pos_actual, destino_intento) || obtener_casilla_intermedia(pos_actual, destino_intento) != -1) {
                intentar_movimiento(args->mesa, pos_actual, destino_intento, puma, -1);
            }
        }
        
        // gana con 6 capturas
        if (args->mesa->capturas_puma >= 6) {
            args->mesa->partida_activa = 0;
        }
    }
    free(args);
    return NULL;
}

void* hilo_perro(void* arg) {
    argumentos_ficha *args = (argumentos_ficha*)arg;
    
    while (args->mesa->partida_activa && servidor_activo) {
        retardo_aleatorio(args->config);
        
        int pos_actual = -1;
        for (int i = 0; i < num_casillas; i++) {
            if (args->mesa->tablero[i].ocupante == perro && args->mesa->tablero[i].id_perro == args->id) {
                pos_actual = i;
                break;
            }
        }
        
        if (pos_actual != -1) {
            int destino_intento = rand() % num_casillas;
            if (es_adyacente(pos_actual, destino_intento)) {
                intentar_movimiento(args->mesa, pos_actual, destino_intento, perro, args->id);
            }
        }
    }
    free(args);
    return NULL;
}

// dibujo en consola
void imprimir_tablero(estado_mesa *mesa, int id_mesa) {
    printf("\n--- ESTADO MESA %d ---\n", id_mesa);
    char c[num_casillas];
    for(int i = 0; i < num_casillas; i++) {
        if(mesa->tablero[i].ocupante == puma) c[i] = 'P';
        else if(mesa->tablero[i].ocupante == perro) c[i] = 'O';
        else c[i] = '.';
    }
    
    printf("        %c        \n", c[27]);
    printf("      %c   %c      \n", c[25], c[26]);
    
    for(int y = 0; y < 5; y++) {
        printf("    ");
        for(int x = 0; x < 5; x++) {
            printf("%c ", c[y * 5 + x]);
        }
        printf("\n");
    }
    printf("Capturas Puma: %d | Perros Vivos: %d\n", mesa->capturas_puma, mesa->perros_vivos);
    printf("----------------------\n");
}

void* hilo_juez(void* arg) {
    argumentos_mesa *args = (argumentos_mesa*)arg;
    estado_mesa mesa;
    
    inicializar_tablero(&mesa); 
    colocar_fichas_iniciales(&mesa, &args->config);
    
    pthread_t thread_puma;
    pthread_t threads_perros[12];
    
    argumentos_ficha *arg_puma = malloc(sizeof(argumentos_ficha));
    arg_puma->id = 0;
    arg_puma->mesa = &mesa;
    arg_puma->config = &args->config;
    pthread_create(&thread_puma, NULL, hilo_puma, arg_puma);
    
    for (int i = 0; i < 12; i++) {
        argumentos_ficha *arg_perro = malloc(sizeof(argumentos_ficha));
        arg_perro->id = i;
        arg_perro->mesa = &mesa;
        arg_perro->config = &args->config;
        pthread_create(&threads_perros[i], NULL, hilo_perro, arg_perro);
    }
    
    while (mesa.partida_activa && servidor_activo) {
        imprimir_tablero(&mesa, args->id_mesa);
        
        if (!mesa.puma_conectado) {
            printf("[MESA %d] Victoria para los perros por abandono del puma.\n", args->id_mesa);
            mesa.partida_activa = 0;
            break;
        }
        if (mesa.perros_conectados < 12) {
            printf("[MESA %d] Victoria para el puma por abandono de un perro.\n", args->id_mesa);
            mesa.partida_activa = 0;
            break;
        }

        sleep(1); 
    }
    
    pthread_join(thread_puma, NULL);
    for (int i = 0; i < 12; i++) {
        pthread_join(threads_perros[i], NULL);
    }
    
    destruir_tablero(&mesa); 
    sem_post(&mesas_sem); 
    free(args);
    return NULL;
}

// ctrl+c
void manejador_sigint(int sig) {
    (void)sig;
    servidor_activo = 0;
}

// libera mutex
void destruir_tablero(estado_mesa *mesa) {
    pthread_mutex_destroy(&mesa->mutex_estado);
    for (int i = 0; i < num_casillas; i++) {
        pthread_mutex_destroy(&mesa->tablero[i].mutex_casilla);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Uso: %s N M [config.txt]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int n = atoi(argv[1]);
    int m = atoi(argv[2]);

    struct sigaction sa;
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    sem_init(&mesas_sem, 0, m);

    config_juego configuracion;
    inicializar_config_default(&configuracion);
    if (argc == 4) {
        leer_config(argv[3], &configuracion);
    }

    int partidas_totales = n / 2; 
    pthread_t jueces[partidas_totales];

    for (int i = 0; i < partidas_totales; i++) {
        if (!servidor_activo) break;

        // el semaforo limita cuantas mesas corren a la vez
        sem_wait(&mesas_sem);

        argumentos_mesa *args = malloc(sizeof(argumentos_mesa));
        args->id_mesa = i + 1;
        args->config = configuracion;

        if (pthread_create(&jueces[i], NULL, hilo_juez, args) != 0) {
            sem_post(&mesas_sem);
            free(args);
        }
    }

    for (int i = 0; i < partidas_totales; i++) {
        if (jueces[i]) pthread_join(jueces[i], NULL);
    }

    sem_destroy(&mesas_sem);
    return EXIT_SUCCESS;
}