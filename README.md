# torneo de komikan concurrente - tarea 2 sistemas operativos

este proyecto implementa un servidor multihilo en c para simular partidas concurrentes del juego mapuche komikan en tiempo real. utiliza primitivas posix (hilos, mutexes y semaforos) para garantizar exclusion mutua, sincronizacion sin turnos y prevencion de deadlocks.

---

## 1. requisitos del sistema y compilacion

el programa esta desarrollado en c17 para entornos unix/linux y requiere las librerias posix de hilos (`pthread`).

### comando de compilacion
para compilar el proyecto aplicando los flags estrictos requeridos:

```bash
gcc -Wall -Wextra -std=c17 -pthread main.c -o komikan
```

---

## 2. ejecucion del servidor

el ejecutable recibe como parametros obligatorios la cantidad total de jugadores (`n`), el limite maximo de mesas simultaneas (`m`) y opcionalmente la ruta a un archivo de configuracion.

```bash
./komikan <n_jugadores> <max_mesas> [archivo_config]
```

### ejemplo de uso
```bash
./komikan 4 2 config.txt
```

---

## 3. formato del archivo de configuracion (`config.txt`)

el archivo de configuracion define los parametros iniciales de retardo, semilla aleatoria y posicion inicial del puma.

### formato:
```text
CONFIG SEED 12345 DELAY_MIN 50 DELAY_MAX 300 PUMA_INIT 2 2
```

### parametros:
- `SEED`: semilla para reproducibilidad de movimientos aleatorios.
- `DELAY_MIN` y `DELAY_MAX`: rango de retardo aleatorio en milisegundos entre cada intento de movimiento de las fichas.
- `PUMA_INIT`: coordenada inicial (x, y) del puma en la cuadricula de 5x5 (por defecto: `2 2`).

---

## 4. arquitectura y decisiones de sincronizacion

### a. emparejamiento y limite de mesas (`sem_t mesas_sem`)
- el proceso principal (`main`) acepta `n` jugadores y los agrupa de a 2 (1 puma y 12 perros por partida).
- la cantidad de mesas activas simultaneamente esta restringida a `m` mediante un semaforo posix (`mesas_sem`). si las `m` mesas estan ocupadas, los pares restantes quedan en espera pasiva (`sem_wait`) sin consumir recursos de cpu.

### b. exclusion mutua y proteccion del tablero
- el tablero posee 28 casillas (25 de la cuadricula 5x5 mas 3 del trono).
- cada casilla posee su propio `pthread_mutex_t`. cuando una ficha intenta moverse o realizar un salto de captura, adquiere los mutexes de las casillas involucradas.

### c. prevencion de deadlocks
- para prevenir deadlocks cuando multiples fichas intentan bloquear casillas adyacentes al mismo tiempo, los mutexes siempre se solicitan e imponen en **orden ascendente segun el indice de la casilla** (`c1 < c2`).

### d. resiliencia y limpieza de recursos
- **manejo de sigint (ctrl+c):** se captura la senal mediante `sigaction` para detener la aceptacion de nuevos jugadores y permitir que el servidor libere mutexes, semaforos y memoria dinamica limpiamente sin dejar recursos huerfanos.
- **desconexion de jugadores:** el hilo juez monitorea el estado de conexion de los participantes. si un hilo aborta o se desconecta, el juez declara la victoria del rival por abandono y libera la mesa para los siguientes jugadores en cola.

---

## 5. estructura del repositorio

- `main.c`: codigo fuente principal con la logica del servidor, hilos de fichas, hilo juez y primitivas posix.
- `config.txt`: archivo de configuracion de parametros iniciales.
- `README.md`: documentacion e instrucciones del proyecto.
- `informe.pdf`: informe de diseno de sincronizacion y analisis de riesgos de concurrencia (maximo 2 paginas).