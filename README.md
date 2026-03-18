# Simple Chat Protocol

Aplicación de chat cliente-servidor en C/C++ que utiliza **sockets**, **multithreading** y **Protocol Buffers** para la serialización de mensajes.

## Estructura del proyecto

```
.
├── protos/
│   ├── common.proto                # StatusEnum compartido
│   ├── cliente-side/               # Mensajes: Cliente → Servidor
│   │   ├── register.proto
│   │   ├── message_general.proto
│   │   ├── message_dm.proto
│   │   ├── change_status.proto
│   │   ├── list_users.proto
│   │   ├── get_user_info.proto
│   │   └── quit.proto
│   └── server-side/                # Mensajes: Servidor → Cliente
│       ├── all_users.proto
│       ├── for_dm.proto
│       ├── broadcast_messages.proto
│       ├── get_user_info_response.proto
│       └── server_response.proto
├── docs/
│   ├── instructions.md             # Requisitos del proyecto
│   └── protocol_standard.md        # Especificación del protocolo
├── .gitignore
└── README.md
```

## Dependencias

- **protoc** (Protocol Buffers compiler) — se necesita para generar los archivos `.pb.h` y `.pb.cc` a partir de los `.proto`.

## Compilar los protos

Desde la raíz del proyecto:

```bash
# Cliente
protoc -I protos/ --cpp_out=. protos/common.proto protos/cliente-side/*.proto

# Servidor
protoc -I protos/ --cpp_out=. protos/common.proto protos/server-side/*.proto
```

Los archivos generados (`.pb.h` / `.pb.cc`) están en `.gitignore` y no se incluyen en el repositorio.

## Protocolo

### Tipo compartido

| Enum `StatusEnum` | Valor |
|---|---|
| ACTIVE | 0 |
| DO_NOT_DISTURB | 1 |
| INVISIBLE | 2 |

### Cliente → Servidor

Todos los mensajes incluyen el campo `ip` del cliente.

| Proto | Descripción | Campos |
|---|---|---|
| `register` | Registro de usuario | username, ip |
| `message_general` | Mensaje al chat general | message, status, username_origin, ip |
| `message_dm` | Mensaje directo | message, status, username_des, ip |
| `change_status` | Cambio de status | status, username, ip |
| `list_users` | Solicitar lista de usuarios | username, ip |
| `get_user_info` | Solicitar info de un usuario | username_des, username, ip |
| `quit` | Desconectarse | quit, ip |

### Servidor → Cliente

| Proto | Descripción | Campos |
|---|---|---|
| `all_users` | Lista de usuarios conectados | usernames[], status[] |
| `for_dm` | DM reenviado al destinatario | username_des, message |
| `broadcast_messages` | Mensaje del chat general | message, username_origin |
| `get_user_info_response` | Respuesta con info de usuario | ip_address, username, status |
| `server_response` | Respuesta general del servidor | status_code, message |

## Ejecución

```bash
# Servidor
./<nombredelservidor> <puertodelservidor>

# Cliente
./<nombredelcliente> <nombredeusuario> <IPdelservidor> <puertodelservidor>
```
