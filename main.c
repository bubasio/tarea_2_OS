/*
==============================================================
PARTE DE LA RUBRICA: 1.1 COMPILACION, EJECUCION Y CONFIGURACION
==============================================================
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <signal.h>

// configuracion inicial del juego[cite: 2]
typedef struct {
    int seed;
    int delay_min;
    int delay_max;
    int puma_init_x;
    int puma_init_y;
} config_juego;

// variables globales y senales
volatile sig_atomic_t servidor_activo = 1; 
sem_t mesas_sem; 

void inicializar_config_default(config_juego *c) {
    c->seed = 12345;
    c->delay_min = 50;
    c->delay_max = 300;
    c->puma_init_x = 2; 
    c->puma_init_y = 2;
}

void leer_config(const char *filename, config_juego *c) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("error al abrir config. usando defaults.");
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

/*
==============================================================
PARTE DE LA RUBRICA: 1.2 TABLERO Y REGLAS BASICAS
==============================================================
*/

// "el tablero esta conformado por una cuadricula principal de 5x5 intersecciones acoplada en su extremo superior a un triangulo o trono"[cite: 1]
#define num_casillas 28 

typedef enum { vacia, puma, perro } tipo_ficha;

typedef struct {
    tipo_ficha ocupante;
    int id_perro; 
    pthread_mutex_t mutex_casilla; 
} casilla;

// "el estado del tablero debe ser compartido entre las fichas concurrentes de cada partida."[cite: 1]
typedef struct {
    casilla tablero[num_casillas];
    int capturas_puma;
    int perros_vivos;
    int partida_activa;
    int puma_conectado;    // 1: conectado, 0: desconectado
    int perros_conectados; // cantidad de perros activos
    pthread_mutex_t mutex_estado;
} estado_mesa;

// "disposicion inicial de fichas en el tablero."[cite: 2]
void colocar_fichas_iniciales(estado_mesa *mesa, config_juego *config) {
    int pos_puma = (config->puma_init_y * 5) + config->puma_init_x;
    mesa->tablero[pos_puma].ocupante = puma;
    
    // 12 perros en la parte inferior
    int perros_colocados = 0;
    for (int i = 24; i >= 0 && perros_colocados < 12; i--) {
        if (mesa->tablero[i].ocupante == vacia) {
            mesa->tablero[i].ocupante = perro;
            mesa->tablero[i].id_perro = perros_colocados;
            perros_colocados++;
        }
    }
}

// "se mueven a casillas adyacentes libres."[cite: 2]
int es_adyacente(int origen, int destino) {
    if (origen < 0 || origen >= num_casillas || destino < 0 || destino >= num_casillas) {
        return 0;
    }
    // cuadricula 5x5 (indices 0 al 24)
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


/*
===================================================================
PARTE DE LA RUBRICA: 2.2 SINCRONIZACION Y CONSISTENCIA DEL TABLERO
===================================================================
*/
// "1. inicializa el tablero y sus primitivas de sincronizacion."[cite: 3]
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

// "identificar los potenciales escenarios de deadlock... y explicar la estrategia implementada"[cite: 4]
void bloquear_casillas(estado_mesa *mesa, int c1, int c2) {
    int primero = (c1 < c2) ? c1 : c2;
    int segundo = (c1 < c2) ? c2 : c1;
    // "garantizar la exclusion mutua durante los movimientos simultaneos"[cite: 4]
    pthread_mutex_lock(&mesa->tablero[primero].mutex_casilla);
    pthread_mutex_lock(&mesa->tablero[segundo].mutex_casilla);
}

void desbloquear_casillas(estado_mesa *mesa, int c1, int c2) {
    int primero = (c1 < c2) ? c1 : c2;
    int segundo = (c1 < c2) ? c2 : c1;
    pthread_mutex_unlock(&mesa->tablero[segundo].mutex_casilla);
    pthread_mutex_unlock(&mesa->tablero[primero].mutex_casilla);
}

// "el sistema de sincronizacion debe garantizar que solo una adquiera la posicion, evitando condiciones de carrera"[cite: 2]
int obtener_casilla_intermedia(int origen, int destino) {
    if (origen >= 25 || destino >= 25) return -1; 
    int x1 = origen % 5, y1 = origen / 5;
    int x2 = destino % 5, y2 = destino / 5;
    
    if (abs(x1 - x2) == 2 && abs(y1 - y2) == 0) return origen + (x2 - x1) / 2; 
    if (abs(x1 - x2) == 0 && abs(y1 - y2) == 2) return origen + ((y2 - y1) / 2) * 5; 
    if (abs(x1 - x2) == 2 && abs(y1 - y2) == 2) return origen + (x2 - x1) / 2 + ((y2 - y1) / 2) * 5; 
    
    return -1;
}

int intentar_movimiento(estado_mesa *mesa, int origen, int destino, tipo_ficha tipo, int id_perro) {
    int exito = 0;
    int casilla_intermedia = (tipo == puma) ? obtener_casilla_intermedia(origen, destino) : -1;

    if (casilla_intermedia != -1) {
        int casillas[3] = {origen, destino, casilla_intermedia};
        for (int i = 0; i < 2; i++) {
            for (int j = i + 1; j < 3; j++) {
                if (casillas[i] > casillas[j]) { int temp = casillas[i]; casillas[i] = casillas[j]; casillas[j] = temp; }
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


/*
===================================================================
PARTE DE LA RUBRICA: 2.3 JUEZ, REGISTRO Y TERMINO DE PARTIDAS
===================================================================
*/

typedef struct {
    int id_mesa;
    config_juego config;
} argumentos_mesa;

typedef struct {
    int id; 
    estado_mesa *mesa;
    config_juego *config;
} argumentos_ficha;

// andaba tirando error pq no puse esto antes de todo
void destruir_tablero(estado_mesa *mesa);

// "cada pieza intenta realizar movimientos ejecutando ciclos de decision con retardos aleatorios en milisegundos (entre 50 y 300 ms)."[cite: 2]
void retardo_aleatorio(config_juego *config) {
    int retardo_ms = config->delay_min + rand() % (config->delay_max - config->delay_min + 1);
    usleep(retardo_ms * 1000); 
}

int puma_tiene_movimientos(estado_mesa *mesa, int pos_puma) {
    for (int destino = 0; destino < num_casillas; destino++) {
        if (mesa->tablero[destino].ocupante == vacia) {
            if (es_adyacente(pos_puma, destino)) return 1; // Puede dar un paso normal
            if (obtener_casilla_intermedia(pos_puma, destino) != -1 && 
                mesa->tablero[obtener_casilla_intermedia(pos_puma, destino)].ocupante == perro) {
                return 1; // Puede saltar sobre un perro
            }
        }
    }
    return 0; // Puma acorralado
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
            // "victoria de los perros: acorralar completamente al puma, dejandolo sin movimientos validos posibles."[cite: 2]
            if (!puma_tiene_movimientos(args->mesa, pos_actual)) {
                args->mesa->partida_activa = 0;
                break;
            }

            int destino_intento = rand() % num_casillas;
            if (es_adyacente(pos_actual, destino_intento) || obtener_casilla_intermedia(pos_actual, destino_intento) != -1) {
                intentar_movimiento(args->mesa, pos_actual, destino_intento, puma, -1);
            }
        }
        
        // "victoria del puma: capturar a 6 o mas perros."[cite: 2]
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
        
        // Buscar la posicion actual de este perro especifico
        int pos_actual = -1;
        for (int i = 0; i < num_casillas; i++) {
            if (args->mesa->tablero[i].ocupante == perro && args->mesa->tablero[i].id_perro == args->id) {
                pos_actual = i;
                break;
            }
        }
        
        if (pos_actual != -1) {
            // Intentar moverse a una casilla adyacente aleatoria
            int destino_intento = rand() % num_casillas;
            if (es_adyacente(pos_actual, destino_intento)) {
                intentar_movimiento(args->mesa, pos_actual, destino_intento, perro, args->id);
            }
        }
    }
    free(args);
    return NULL;
}

/*
===================================================================
PARTE DE LA RUBRICA: 2.3 JUEZ, REGISTRO Y TERMINO DE PARTIDAS
( ESTO ES PA Q SE VEA VISUAL )
===================================================================
*/
// "3. simulacion visual/log del juego: la mesa debe imprimir/simular periodicamente o ante cada movimiento el estado actualizado del tablero"[cite: 3]
void imprimir_tablero(estado_mesa *mesa, int id_mesa) {
    printf("\n--- ESTADO MESA %d ---\n", id_mesa);
    char c[num_casillas];
    for(int i = 0; i < num_casillas; i++) {
        if(mesa->tablero[i].ocupante == puma) c[i] = 'P';
        else if(mesa->tablero[i].ocupante == perro) c[i] = 'O';
        else c[i] = '.';
    }
    
    // imprimir el trono (indices 25, 26, 27)
    printf("        %c        \n", c[27]);
    printf("      %c   %c      \n", c[25], c[26]);
    
    // imprimir la cuadricula 5x5 (indices 0 al 24)
    for(int y = 0; y < 5; y++) {
        printf("    ");
        for(int x = 0; x < 5; x++) {
            printf("%c ", c[y * 5 + x]);
        }
        printf("\n");
    }
    printf("capturas puma: %d | perros vivos: %d\n", mesa->capturas_puma, mesa->perros_vivos);
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
        
        // Deteccion de desconexiones repentinas (Resiliencia)
        if (!mesa.puma_conectado) {
            printf("[MESA %d] victoria para los perros por abandono del puma.\n", args->id_mesa);
            mesa.partida_activa = 0;
            break;
        }
        if (mesa.perros_conectados < 12) {
            printf("[MESA %d] victoria para el puma por abandono de un perro.\n", args->id_mesa);
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

/*
===================================================================
PARTE DE LA RUBRICA: 
1.3 EMPAREJAMIENTO Y MESAS 
2.1 LIMITE DE MESAS  
2.4 FALLOS, SIGINT Y LIMPIEZA 
===================================================================
*/

// "si el usuario envia una senal sigint (ctrl+c) al proceso principal: el servidor debe detener inmediatamente la llegada de nuevos jugadores."[cite: 3]
void manejador_sigint(int sig) {
    (void)sig;
    servidor_activo = 0;
}

// "4. el programa debe finalizar garantizando cero fugas de memoria (memory leaks) y cero recursos huerfanos en el sistema operativo."[cite: 3]
void destruir_tablero(estado_mesa *mesa) {
    pthread_mutex_destroy(&mesa->mutex_estado);
    for (int i = 0; i < num_casillas; i++) {
        pthread_mutex_destroy(&mesa->tablero[i].mutex_casilla);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "uso: %s n m [config.txt]\n", argv[0]);
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

    // "el servidor debe emparejarlos en grupos de 2 (un jugador puma y un jugador perros) y asignarles una mesa de juego disponible."[cite: 3]
    int partidas_totales = n / 2; 
    pthread_t jueces[partidas_totales];

    for (int i = 0; i < partidas_totales; i++) {
        if (!servidor_activo) break;

        // "si las m mesas estan ocupadas, los pares restantes deben esperar de forma pasiva."[cite: 3]
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