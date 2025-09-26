import yaml
import sys

def check_yaml_file(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            yaml.safe_load(f)
        print(f"✓ YAML语法检查通过！文件 '{file_path}' 结构有效。")
        return 0
    except yaml.YAMLError as e:
        print(f"✗ YAML语法检查失败！文件 '{file_path}' 存在语法错误：")
        print(f"  错误信息: {e}")
        return 1
    except Exception as e:
        print(f"✗ 检查过程中发生错误：")
        print(f"  错误信息: {e}")
        return 2

if __name__ == "__main__":
    file_path = ".github/workflows/build_windows.yml"
    sys.exit(check_yaml_file(file_path))