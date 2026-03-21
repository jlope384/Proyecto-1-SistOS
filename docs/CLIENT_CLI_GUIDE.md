# Cliente CLI para Chat de Sistemas Operativos

## Descripción
`client_cli` es un cliente de chat basado en línea de comandos diseñado para funcionar en WSL (Windows Subsystem for Linux) y Linux. Proporciona una interfaz interactiva con soporte para comandos.

## Compilación

```bash
make client_cli
```

O para compilar todo:

```bash
make all
```

## Uso

```bash
./client_cli <username> <IP_servidor> <puerto>
```

### Ejemplo:

```bash
./client_cli alice 127.0.0.1 5000
./client_cli bob localhost 5000
./client_cli charlie 192.168.1.100 5000
```

## Comandos disponibles

### `/dm <usuario> <mensaje>`
Envía un mensaje directo a otro usuario.

```
> /dm bob Hola Bob, ¿cómo estás?
[DM enviado a bob]: Hola Bob, ¿cómo estás?
```

### `/state <estado>`
Cambia tu estado actual. Los estados disponibles son:
- `active` - Activo y disponible
- `dnd` - Do Not Disturb (No molestar)
- `invisible` - Invisible

```
> /state dnd
[STATUS] Estado cambiado a: DO_NOT_DISTURB

> /state active
[STATUS] Estado cambiado a: ACTIVE
```

### `/users`
Lista todos los usuarios en línea con sus estados actuales.

```
> /users

── Usuarios en línea (3) ──
  • alice [ACTIVE]
  • bob [DO_NOT_DISTURB]
  • charlie [ACTIVE]
```

### `/info <usuario>`
Obtiene información detallada de un usuario específico.

```
> /info bob

── Información de usuario ──
  Usuario: bob
  Estado: DO_NOT_DISTURB
  IP: 192.168.1.101
```

### `/help`
Muestra la lista de todos los comandos disponibles.

```
> /help

════════════════════════════════════════════════
Comandos disponibles:
════════════════════════════════════════════════
  /dm <usuario> <mensaje>  - Enviar mensaje directo
  /state <estado>          - Cambiar estado
                             (active, dnd, invisible)
  /users                   - Listar usuarios en línea
  /info <usuario>          - Obtener info de usuario
  /help                    - Mostrar esta ayuda
  /quit                    - Salir
  <mensaje>                - Enviar mensaje general
════════════════════════════════════════════════
```

### `/quit`
Desconecta del servidor y sale de la aplicación.

```
> /quit
[CLIENT] Desconectado.
```

### Mensaje general (Broadcast)
Cualquier texto que no comience con `/` se envía como mensaje general visible para todos los usuarios.

```
> Hola a todos, ¿cómo están?
[alice] Hola a todos, ¿cómo están?

> Buenos días
[alice] Buenos días
```

## Ejemplo de sesión

```bash
$ ./client_cli alice 127.0.0.1 5000
[CLIENT] IP local: 192.168.1.100
[CLIENT] Conectado a 127.0.0.1:5000
[CLIENT] Registrado como 'alice'.
[CLIENT] Escribe /help para ver los comandos.

> /users
── Usuarios en línea (2) ──
  • alice [ACTIVE]
  • bob [ACTIVE]
> /dm bob ¡Hola Bob!
[DM enviado a bob]: ¡Hola Bob!
> /state dnd
[STATUS] Estado cambiado a: DO_NOT_DISTURB
> Mensaje general para todos
[alice] Mensaje general para todos
> /info bob
── Información de usuario ──
  Usuario: bob
  Estado: ACTIVE
  IP: 192.168.1.101
> /quit
[CLIENT] Desconectado.
```

## Características

- ✅ **Interfaz interactiva** - Prompt de línea de comandos simple
- ✅ **Mensajes generales** - Broadcast para todos los usuarios
- ✅ **Mensajes directos** - DM privados entre usuarios
- ✅ **Gestión de estado** - Cambiar entre ACTIVE, DND, INVISIBLE
- ✅ **Lista de usuarios** - Ver quiénes están en línea y sus estados
- ✅ **Información de usuario** - Consultar detalles de otros usuarios
- ✅ **Thread seguro** - Recepción de mensajes sin bloquear entrada del usuario
- ✅ **Manejo de conexión** - Desconexión limpia del servidor

## Notas

- El cliente es **thread-safe** - los mensajes del servidor se pueden recibir mientras escribes
- Los comandos son **case-insensitive** para los estados (active, ACTIVE, Active funcionan igual)
- El servidor debe estar ejecutándose antes de iniciar el cliente
- El cliente se intenta conectar a la IP y puerto especificados
