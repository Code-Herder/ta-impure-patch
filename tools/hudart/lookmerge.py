"""File one look's freshly cut parts into a kit under the look's names.
usage: lookmerge.py <cut dir> <look> <kit dir>   -> <kit dir>/<part>@<look>.png and its kit.json entries"""
import json, os, shutil, sys
src, look, dst = sys.argv[1:4]
S = json.load(open(os.path.join(src, 'kit.json'))); kp = os.path.join(dst, 'kit.json'); K = json.load(open(kp))
for k, e in S.items():
    shutil.copy(os.path.join(src, k + '.png'), os.path.join(dst, f'{k}@{look}.png')); K[f'{k}@{look}'] = e
json.dump(K, open(kp, 'w'), indent=1)
print(look, sorted(S))
