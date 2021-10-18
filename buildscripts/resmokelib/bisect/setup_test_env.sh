set -e
"$1" "$2" setup-multiversion -ec "$3" -db -da -i build/resmoke-bisect -l build/resmoke-bisect -v "$4" "$5"
mv build/resmoke-bisect/"$5" build/resmoke-bisect/mongo_repo
"$1" -m venv build/resmoke-bisect/bisect_venv
source build/resmoke-bisect/bisect_venv/bin/activate
"$1" -m pip install --upgrade pip
"$1" -m pip install -r etc/pip/dev-requirements.txt
deactivate
