from traceback import print_exc
from itertools import takewhile
from glob import glob
from subprocess import check_output, run, STDOUT, CalledProcessError, TimeoutExpired
from re import search
from multiprocessing import Pool
import os
import sys

repeats = int(sys.argv[1]) if len(sys.argv) > 1 else 1
memcheck = sys.argv[2] == '--memcheck' if len(sys.argv) > 2 else False

if run(["cmake", "."]).returncode != 0 or run("make").returncode != 0:
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

def exp_parallel(out, strs):
    if len(strs) == 0:
        return out
    indices = [0] * len(strs)
    for i in range(len(out)):
        car = out[i]
        for j in range(len(strs)):
            str = strs[j]
            str_len = len(str)
            index = indices[j]
            if index == str_len:
                continue
            str_car = str[index]
            if car == str_car:
                break
        else:
            continue
        return out[i:]

def exec_test(test_file, num):
    test_name = test_file[6:-5]
    test_error_file = "tests/%s-%i.error" % (test_name, num)
    try:
        # Running test
        out = check_output(
            "cat %s | valgrind --log-file=%s \
                               --error-exitcode=1 \
                               --leak-check=full \
                               --show-leak-kinds=all \
                               ./TP2" % (test_file, test_error_file)
            if memcheck else
            "cat %s | ./TP2" % test_file,
            stderr=STDOUT,
            shell=True,
            timeout=10).decode()
        # Getting expect
        with open('tests/%s.expect' % test_name, 'r') as file:
            expects_str = file.read()
            expects = [str.split(']\n') for str in expects_str.split('[') if str != '']
        # Checking expect
        to_check = out
        for i in range(0, len(expects)):
            [expect_type, expect_value] = expects[i]
            if expect_type == 'expect':
                to_check = exp(to_check, expect_value)
            elif expect_type == 'concurrent' or expect_type == 'parallel':
                tests = takewhile(lambda e: e[0] == 'and', expects[i+1:])
                sub_expects = [expect_value] + [test[1] for test in tests]
                to_check = (exp_concurrent if expect_type == 'concurrent' else exp_parallel)(to_check, sub_expects)
            if to_check == None:
                break
        if to_check == None:
            print("\033[0;31mTest %s not ok, got:\033[0m\n%s\033[0;31mWhen expecting:\033[0m\n%s" % (test_name, out, expects_str))
    except CalledProcessError as e:
        print("\033[0;31mTest %s not ok\033[0m" % test_name)
        with open(test_error_file, 'r') as file:
            print(file.read())
    except TimeoutExpired as e:
        print("\033[0;31mTest %s not ok: took too long\033[0m" % test_name)
    except Exception as e:
        print("\033[0;31mTest %s not ok\033[0m" % test_name)
        print_exc()

test_files = glob('tests/*.test')
print("Checking each test %i times ...\n\033[0;33m%s\033[0m" % (repeats, '\n'.join(test_files)))
pool = Pool(os.cpu_count())
pool.starmap(exec_test, [(f, i) for i in range(repeats) for f in test_files])

print("Tests done")
