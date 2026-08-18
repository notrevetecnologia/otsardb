# OtsarDB Quickstart (for end users)

OtsarDB is a single-binary SQL database. You install **one program** — no
SQLite, no server, no dependencies.

## 1. Install

Download the archive for your OS from the GitHub Releases page
(`otsardb-Linux-<version>.tar.gz`, `otsardb-macOS-<version>.tar.gz`,
`otsardb-Windows-<version>.zip`) and extract it. The archive contains
`otsardb` (or `otsardb.exe`), the public header, the license and this README.

```bash
# Linux/macOS
tar -xzf otsardb-Linux-v0.1.0.tar.gz
sudo mv otsardb /usr/local/bin/

# Windows: unzip and add the folder to PATH
```

Verify:

```bash
otsardb --version
```

## 2. Use it as a normal database (no configuration)

```bash
# Create/open a local database file
otsardb demo.db "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT);"
otsardb demo.db "INSERT INTO users(name) VALUES('João');"
otsardb demo.db "SELECT * FROM users;"

# Interactive shell
otsardb demo.db
#   otsardb> SELECT * FROM users;
```

This works exactly like SQLite — nothing to configure.

## 3. Use S3 as the database location (cloud-native)

OtsarDB can treat an S3-compatible bucket as the authoritative database
location. Local files are only a disposable cache.

```bash
otsardb s3://my-bucket/company/database
```

### 3.1 Credentials

OtsarDB resolves credentials automatically, in this order:

1. **Explicit OtsarDB variables** (highest priority):

   ```bash
   export OTSARDB_S3_ACCESS_KEY="<your-access-key-id>"
   export OTSARDB_S3_SECRET_KEY="..."
   export OTSARDB_S3_SESSION_TOKEN="..."      # optional
   ```

2. **Standard AWS environment variables** (what the AWS CLI uses):

   ```bash
   export AWS_ACCESS_KEY_ID="<your-access-key-id>"
   export AWS_SECRET_ACCESS_KEY="..."
   export AWS_DEFAULT_REGION="us-east-1"
   ```

3. **AWS credentials file** (`~/.aws/credentials`):

   ```ini
   [default]
   aws_access_key_id = <your-access-key-id>
   aws_secret_access_key = ...
   ```

4. **AWS IAM roles** — on EC2/ECS, OtsarDB automatically uses the instance
   profile or task role. No configuration needed.

**If you already use the AWS CLI on your machine, you need to configure
NOTHING for credentials.**

### 3.2 Endpoint and region

- **AWS S3**: nothing to set (defaults to `s3.amazonaws.com`). Region comes
  from `AWS_DEFAULT_REGION` or defaults to `us-east-1`.
- **Other S3-compatible providers** (MinIO, EVEO, Magalu, Wasabi, R2, B2...):

  ```bash
  export OTSARDB_S3_ENDPOINT="https://s3.example.com"
  export OTSARDB_S3_REGION="region-1"
  ```

### 3.3 Example: writing and reopening from S3

```bash
# Write from machine A
otsardb s3://my-bucket/company/orders "CREATE TABLE orders(id INTEGER PRIMARY KEY, total REAL); INSERT INTO orders(total) VALUES(99.9);"

# Reopen from machine B — zero local state, data comes from S3
otsardb s3://my-bucket/company/orders "SELECT * FROM orders;"
```

The S3 database survives the machine being destroyed.

## 4. Operational notes

- `OTSARDB_CACHE_DIR` optionally selects where disposable local cache files
  live (default: system temp). Safe to delete anytime.
- `OTSARDB_S3_PREFIX` optionally places all OtsarDB objects under a provider-side
  prefix.
- `OTSARDB_AUTO_CHECKPOINT_COMMITS=N` (default 1000) automatically materializes
  a checkpoint after N commits, keeping recovery fast as history grows.
- `OTSARDB_AUTO_CHECKPOINT=0` disables automatic checkpoints.
- Durability: a successful `COMMIT` means the data is durably published to
  object storage.

## 5. What you do NOT need

- No SQLite installation (OtsarDB is the database).
- No server daemon.
- No database configuration files.
- No `init`/setup step for S3 — the bucket just needs to exist and your
  credentials must allow `GetObject`/`PutObject`/`DeleteObject`/`ListBucket`
  on it.
