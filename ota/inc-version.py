import os
import re
import sys

semver_regex = re.compile(r'^(?P<start>\d+\.\d+\.\d+-[^+0-9]+)(?P<prerelease>\d+)')

def main():
    project_dir = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(__file__)), '..'))
    version_path = os.path.join(project_dir, 'version.txt')

    with open(version_path, 'r') as f:
        version = f.readline()

    m = semver_regex.match(version)
    if m:
        prerelease = m.group('prerelease')
        prerelease_len = len(prerelease)
        prerelease_num = int(prerelease)
        prerelease_num += 1
        new_version = ("{}{:0" + str(prerelease_len) + "d}").format(m.group('start'), prerelease_num)

        with open(version_path, 'w') as f:
            f.write(new_version)
            f.flush()

if __name__ == "__main__":
    main()
