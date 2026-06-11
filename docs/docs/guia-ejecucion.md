# Guía de ejecución — Sistema de Matrícula Distribuido

## Requisitos previos

| Herramienta | Mínimo | Verificar con |
|-------------|--------|---------------|
| GCC | 9+ | `gcc --version` |
| GNU Make | cualquiera | `make --version` |
| Python 3 | 3.6+ | `python3 --version` |
| POSIX mqueue montada | — | `ls /dev/mqueue/` (debe existir el directorio) |

> En WSL2 la cola POSIX requiere que `/dev/mqueue` esté montada. Si no existe, ejecutá:
> ```bash
> sudo mkdir -p /dev/mqueue
> sudo mount -t mqueue none /dev/mqueue
> ```

---

## 1. Compilar

```bash
make          # compila servidor y cliente en build/
```

Targets disponibles:

```bash
make servidor   # solo el servidor
make cliente    # solo el cliente
make clean      # borra build/, servidor.log, datos/ y la cola POSIX
```

Los binarios quedan en `build/servidor` y `build/cliente`.

---

## 2. Ejecutar el servidor

Abrí una terminal y corré:

```bash
./build/servidor
```

El servidor:
- Escucha conexiones TCP en el puerto **5001**.
- Crea la cola POSIX en `/dev/mqueue/matricula_srv`.
- Crea los archivos de datos en `datos/` la primera vez que se inserta un registro.
- Escribe eventos en `servidor.log`.

No produce salida en consola salvo mensajes de error críticos; todo va al log.

---

## 3. Ejecutar el cliente

En una **segunda terminal**, con el servidor ya corriendo:

```bash
./build/cliente
```

El cliente abre un menú interactivo:

```
=== Sistema de Matricula ===
1. Estudiantes
2. Profesores
3. Materias
4. Matricula
5. Salir
```

Cada entidad tiene un submenú **Ingresar / Buscar / Regresar**. Los datos se envían al servidor via TCP y la respuesta se muestra en pantalla.

### Campos por entidad

| Entidad | Campos obligatorios |
|---------|---------------------|
| Estudiante | Cédula (≤15 chars), nombre, apellido, email |
| Profesor | Cédula, nombre, apellido, departamento, email |
| Materia | Código (≤15 chars), nombre, descripción, créditos, cédula del profesor |
| Matrícula | Cédula estudiante, código materia, periodo (ej. `2025-1`) |

### Reglas de integridad referencial

Al insertar una **matrícula**, el servidor valida que:
- La cédula del estudiante exista en `datos/estudiantes.dat`.
- El código de materia exista en `datos/materias.dat`.
- La cédula del profesor asignado a esa materia exista en `datos/profesores.dat`.

Si falla alguna validación, el servidor devuelve el motivo específico.

---

## 4. Consultar el log

```bash
cat servidor.log          # ver todos los eventos
tail -f servidor.log      # seguir en tiempo real
```

Cada línea tiene el formato:

```
[INFO ] <mensaje>
[ERROR] <mensaje>
```

---

## 5. Archivos de datos

Los registros se persisten en texto plano dentro de `datos/`:

```
datos/
├── estudiantes.dat
├── profesores.dat
├── materias.dat
└── matriculas.dat
```

Cada línea es un registro con campos separados por `|`. Se pueden inspeccionar directamente:

```bash
cat datos/estudiantes.dat
```

---

## 6. Ejecutar la suite de pruebas automáticas

El script `prueba.sh` verifica la rúbrica completa sin intervención manual:

```bash
bash prueba.sh
```

Qué verifica:

1. Compilación sin warnings (`-Wall -Wextra`).
2. Presencia de símbolos POSIX mqueue en el binario.
3. Arranque correcto del servidor.
4. Existencia de `/dev/mqueue/matricula_srv`.
5. Protocolo TCP/IP e inserción de las 4 entidades.
6. Detección de duplicados.
7. Búsqueda de entidades existentes e inexistentes.
8. Integridad referencial (matrícula sin estudiante/materia/profesor).
9. Archivos de datos en disco.
10. Log por pipe interno.
11. Conteo de hilos por operación.
12. Inserciones concurrentes con mutex por entidad.

> El script lanza y detiene el servidor automáticamente. No es necesario tenerlo corriendo antes.

---

## 7. Flujo de prueba manual rápida

```bash
# Terminal 1
./build/servidor

# Terminal 2 — insertar un profesor
./build/cliente
# Elegir: 2 (Profesores) → 1 (Ingresar)
# Cédula: P001
# Nombre: Ana
# Apellido: Rojas
# Departamento: Informatica
# Email: ana@una.ac.cr

# Insertar una materia
# Elegir: 3 (Materias) → 1 (Ingresar)
# Código: MAT101  Nombre: Algoritmos  Descripción: Fundamentos
# Créditos: 4  Cédula profesor: P001

# Insertar un estudiante
# Elegir: 1 (Estudiantes) → 1 (Ingresar)
# Cédula: E001  Nombre: Carlos  Apellido: Mora  Email: cmora@una.ac.cr

# Matricular
# Elegir: 4 (Matrícula) → 1 (Ingresar)
# Cédula estudiante: E001  Código materia: MAT101  Periodo: 2025-1
```

---

## 8. Detener el servidor

```bash
# Ctrl+C en la terminal del servidor, o:
kill $(pgrep servidor)
```

Para limpiar todo y comenzar desde cero:

```bash
make clean
```

Esto elimina los binarios, los archivos de datos, el log y la cola POSIX (`/dev/mqueue/matricula_srv`).
