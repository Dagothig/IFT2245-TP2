from traceback import print_exc
from itertools import takewhile
from glob import glob
from subprocess import check_output, run, STDOUT, CalledProcessError, TimeoutExpired
from re import search
from multiprocessing import Pool
import os

res = run("make")
if res.returncode != 0:
    exit()

def exp(out, str):
    if out != None and out[:len(str)] == str:
        return out[len(str):]

def exp_concurrent(out, strs):
    if len(strs) == 0:
        return out
    for str in strs:
        found = exp(out, str)
        if found != None:
            strs.remove(str)
            return exp_concurrent(found, strs)

def exec_test(test_file):
    test_name = test_file[6:-5]
    test_error_file = "tests/%s.error" % test_name
    try:
        # Running test
        out = check_output(
            "cat %s | valgrind --log-file=%s \
                               --error-exitcode=1 \
                               --leak-check=full \
                               --show-leak-kinds=all \
                               ./TP2" % (test_file, test_error_file),
            stderr=STDOUT,
            shell=True,
            timeout=5).decode()
        # Getting expect
        with open('tests/%s.expect' % test_name, 'r') as file:
            expects = [str.split(']\n') for str in file.read().split('[') if str != '']
        # Checking expect
        to_check = out
        for i in range(0, len(expects)):
            [expect_type, expect_value] = expects[i]
            if expect_type == 'expect':
                to_check = exp(to_check, expect_value)
            elif expect_type == 'concurrent':
                tests = takewhile(lambda e: e[0] == 'and', expects[i+1:])
                to_check = exp_concurrent(to_check, [expect_value] + [test[1] for test in tests])
            if to_check == None:
                break
        if to_check == None:
            print("Test %s not ok, got:\n%sWhen expecting:\n%s" % (test_name, out, expects))
        else:
            print("Test %s ok" % test_name)
    except CalledProcessError as e:
        print("Test %s not ok" % test_name)
        with open(test_error_file, 'r') as file:
            print(file.read())
    except TimeoutExpired as e:
        print("Test %s not ok: took too long" % test_name)
    except Exception as e:
        print("Test %s not ok" % test_name)
        print_exc()

test_files = glob('tests/*.test')
pool = Pool(os.cpu_count() * 2)
pool.map(exec_test, test_files)

print("Tests done")
