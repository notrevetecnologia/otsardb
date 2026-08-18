# OtsarDB Beta — Início Rápido em 5 Minutos / 5-Minute Quickstart

> Version: current Windows x64 beta package; package contents and the
> browser-first Studio path are governed by the release documentation.
> Every claim below was observed on 2026-08-10 against local MinIO with the
> packaged binary (OBSERVED). Everything else is marked accordingly.

---

## PT-BR — Primeiros 5 minutos

### Pré-requisitos (2 opções — escolha UMA)

| Opção | O que precisa |
|---|---|
| **A. Conta S3 de verdade** | Qualquer endpoint S3-compatível: Wasabi, Magalu Cloud, Cloudflare R2, ou MinIO na nuvem. Crie um bucket e uma chave de acesso (access key / secret key). |
| **B. MinIO local (5 min, grátis)** | Baixe o MinIO de `https://min.io` para Windows e rode em um terminal: `minio.exe server C:\minio-data`. Acesse `http://127.0.0.1:9000`, crie um bucket e gere uma chave. |

### Passo 0 — Descompactar e testar

Descompacte o zip em qualquer pasta (ex.: `$env:USERPROFILE\my-app-beta`) e verifique:

```powershell
cd $env:USERPROFILE\my-app-beta
.\otsardb.exe --version
```

Esperado: `otsardb 0.1.0-dev (engine base 3.53.4)`. Sem instalação, sem PATH — o pacote é standalone (DLLs inclusas; precisa de Windows 10/11 x64).

### Passo 1 — Configurar as variáveis de ambiente

```powershell
$env:OTSARDB_S3_ENDPOINT   = "http://127.0.0.1:9000"      # ou https://s3.us-east-1.wasabisys.com, etc.
$env:OTSARDB_S3_REGION     = "us-east-1"                  # consulte seu provedor
$env:OTSARDB_S3_ACCESS_KEY = "SEU-ACCESS-KEY"
$env:OTSARDB_S3_SECRET_KEY = "SUA-SECRET-KEY"
```

O bucket vai no alvo: `s3://SEU-BUCKET/meu-banco`. (As variáveis padrão da AWS — `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION` — também são aceitas.)

### Passo 2 — Os 4 comandos

Cada invocação é uma sessão: abre, executa e **fecha automaticamente** ao terminar (o CLI é single-shot; o "close" é o fim do processo).

```powershell
# 1) CREATE TABLE
.\otsardb.exe s3://SEU-BUCKET/meu-banco "CREATE TABLE contatos(id INTEGER PRIMARY KEY, nome TEXT, email TEXT);"

# 2) INSERT
.\otsardb.exe s3://SEU-BUCKET/meu-banco "INSERT INTO contatos VALUES(1,'Beta','beta@example.com'),(2,'Otsar','otsar@example.com');"

# 3) SELECT
.\otsardb.exe s3://SEU-BUCKET/meu-banco "SELECT id, nome FROM contatos;"
```

Saída esperada: `1 | Beta` e `2 | Otsar`. (3) **fecha** a sessão quando o processo termina.

### Passo 3 — Provar os dados em "outra máquina"

Agora simule um segundo computador: diretório de cache vazio (ou nenhum). Os dados vivem **no S3**, não no disco local.

```powershell
$env:OTSARDB_CACHE_DIR = "$env:USERPROFILE\my-app-beta\cache-zerada"   # pasta nova/vazia = zero estado local
.\otsardb.exe s3://SEU-BUCKET/meu-banco "SELECT COUNT(*) FROM contatos;"
```

Esperado: `2`. Os mesmos comandos em qualquer outra máquina com as mesmas credenciais retornam o mesmo resultado — recuperação completa a partir do objeto storage (OBSERVED em MinIO local, 2026-08-10).

### Passo 4 — Provedores sem CAS (Wasabi, Magalu): modo single-writer

Wasabi e Magalu **não** implementam escrita condicional (CAS). Neles, adicione:

```powershell
$env:OTSARDB_S3_SINGLE_WRITER = "1"   # você assume: apenas UM escritor por banco
```

Sem isso, o OtsarDB recusa a escrita com uma mensagem clara (fail-closed). MinIO e Cloudflare R2 suportam CAS e não precisam dessa variável.

---

## EN — First 5 minutes

### Prerequisites (pick ONE)

| Option | What you need |
|---|---|
| **A. Real S3 account** | Any S3-compatible endpoint: Wasabi, Magalu Cloud, Cloudflare R2, or cloud MinIO. Create a bucket and an access key pair. |
| **B. Local MinIO (5 min, free)** | Download MinIO for Windows from `https://min.io`, run `minio.exe server C:\minio-data` in a terminal, open `http://127.0.0.1:9000`, create a bucket and a key. |

### Step 0 — Unzip and check

```powershell
cd $env:USERPROFILE\my-app-beta
.\otsardb.exe --version
```

Expected: `otsardb 0.1.0-dev (engine base 3.53.4)`. No install, no PATH — the package is standalone (DLLs included; requires Windows 10/11 x64).

### Step 1 — Environment variables

```powershell
$env:OTSARDB_S3_ENDPOINT   = "http://127.0.0.1:9000"      # or https://s3.us-east-1.wasabisys.com, etc.
$env:OTSARDB_S3_REGION     = "us-east-1"                  # check your provider
$env:OTSARDB_S3_ACCESS_KEY = "YOUR-ACCESS-KEY"
$env:OTSARDB_S3_SECRET_KEY = "YOUR-SECRET-KEY"
```

The bucket goes in the target: `s3://YOUR-BUCKET/my-db`. (AWS-standard env names — `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION` — are also accepted.)

### Step 2 — The 4 commands

Each invocation is one session: it opens, executes and **closes automatically** when it finishes (single-shot CLI; "close" = process exit).

```powershell
# 1) CREATE TABLE
.\otsardb.exe s3://YOUR-BUCKET/my-db "CREATE TABLE contacts(id INTEGER PRIMARY KEY, name TEXT, email TEXT);"

# 2) INSERT
.\otsardb.exe s3://YOUR-BUCKET/my-db "INSERT INTO contacts VALUES(1,'Beta','beta@example.com'),(2,'Otsar','otsar@example.com');"

# 3) SELECT
.\otsardb.exe s3://YOUR-BUCKET/my-db "SELECT id, name FROM contacts;"
```

Expected output: `1 | Beta` and `2 | Otsar`. (3) **closes** the session when the process exits.

### Step 3 — Prove the data on "another machine"

Simulate a second computer: empty (or no) cache directory. Data lives **in S3**, not on local disk.

```powershell
$env:OTSARDB_CACHE_DIR = "$env:USERPROFILE\my-app-beta\empty-cache"      # fresh/empty dir = zero local state
.\otsardb.exe s3://YOUR-BUCKET/my-db "SELECT COUNT(*) FROM contacts;"
```

Expected: `2`. The same commands on any other machine with the same credentials return the same result — full recovery from object storage (OBSERVED on local MinIO, 2026-08-10).

### Step 4 — Providers without CAS (Wasabi, Magalu): single-writer mode

Wasabi and Magalu do **not** implement conditional writes (CAS). On them, add:

```powershell
$env:OTSARDB_S3_SINGLE_WRITER = "1"   # you assert: only ONE writer per database
```

Without it OtsarDB refuses to write with a clear message (fail-closed). MinIO and Cloudflare R2 support CAS and do not need this variable.

---

## Leia antes de avaliar / Read before evaluating

- **`KNOWN_LIMITATIONS.md`** (same folder, also inside this package): segment
  encryption is opt-in and must be configured before writing, forwarding is
  loopback-only (no TLS/auth — never expose it on a network), and recovery
  objectives are scoped to the tested storage/provider path. Read it —
  honesty is a feature.
- For the full product guide (all env vars, credential order, operational settings): `QUICKSTART.md` in the repo.
