#!/bin/sh
# Reset to a clean demo state
#
# WARNING: this permanently deletes ~/.mysh (all packages installed with
# `pkg install`) and the repository's ./tmp directory, then recreates the
# demo package source tree under ./tmp.

# Resolve ./tmp against the repository root; abort if that fails.
cd "$(dirname "$0")/.." || exit 1

rm -rf ~/.mysh
rm -rf ./tmp
mkdir -p ./tmp

# Recreate the source tree
mkdir -p ./tmp/hellopkg/bin
cat > ./tmp/hellopkg/pkg.json <<'EOF'
{
  "name": "hello",
  "version": "1.0.0",
  "description": "Demo hello command",
  "files": ["bin/hello"],
  "docs": []
}
EOF
cat > ./tmp/hellopkg/bin/hello <<'EOF'
#!/bin/sh
echo "hello from pkg"
EOF
chmod +x ./tmp/hellopkg/bin/hello

# Pre-build a backup tarball in case live build fails
tar -czf ./tmp/hello-1.0.0-backup.tar.gz -C ./tmp/hellopkg .

echo "Demo state reset."
echo "  Source tree:    ./tmp/hellopkg"
echo "  Backup tarball: ./tmp/hello-1.0.0-backup.tar.gz"