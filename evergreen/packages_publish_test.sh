DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

. ./notary_env.sh

set -o errexit
set -o verbose

CURATOR_RELEASE=${curator_release}
curl -L -O http://boxes.10gen.com/build/curator/curator-dist-rhel70-$CURATOR_RELEASE.tar.gz
tar -zxvf curator-dist-rhel70-$CURATOR_RELEASE.tar.gz
NOTARY_KEY_NAME=${signing_key_name_test} NOTARY_TOKEN=${signing_auth_token_test} BARQUE_API_KEY=${barque_api_key_test} BARQUE_USERNAME=${barque_user_test} ./curator repo submit --service ${barque_url_test} --config ./etc/repo_config_test.yaml --distro ${packager_distro} --edition ${repo_edition} --version ${version} --arch ${packager_arch} --packages https://s3.amazonaws.com/mciuploads/${project}/${build_variant}/${revision}/artifacts/${build_id}-packages.tgz
