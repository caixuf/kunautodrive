import re

CN_NUM = {
    '零': 0, '一': 1, '二': 2, '两': 2, '三': 3, '四': 4, '五': 5,
    '六': 6, '七': 7, '八': 8, '九': 9, '十': 10
}

def chinese_to_arabic(s):
    """鲁棒的中文/阿拉伯数字转换"""
    s = s.strip()
    try:
        return float(s)
    except ValueError:
        pass
    
    # 简单汉字数字
    if s in CN_NUM:
        return float(CN_NUM[s])
    
    # 复合中文数字如 '二十五', '一百', '两百五十'
    total = 0
    temp = 0
    for char in s:
        if char in CN_NUM:
            temp = CN_NUM[char]
        elif char == '十':
            total += (temp if temp != 0 else 1) * 10
            temp = 0
        elif char == '百':
            total += (temp if temp != 0 else 1) * 100
            temp = 0
        elif char == '千':
            total += (temp if temp != 0 else 1) * 1000
            temp = 0
        elif char == '万':
            total += (temp if temp != 0 else 1) * 10000
            temp = 0
    total += temp
    return float(total) if total > 0 or s == '零' else None

def parse_natural_arithmetic(text):
    """解析中文/英文自然语言算术表达，如'五除2'、'17加9'、'100减去25'、'三乘四'"""
    t = text.strip().replace("等于多少", "").replace("等于几", "").replace("是多少", "").replace("计算", "").replace("算一下", "").replace("？", "").replace("?", "").strip()
    
    pattern = r"([0-9\.\u4e00-\u9fa5]+)\s*(加上|加|\+|减去|减|\-|乘以|乘上|乘|\*|[xX]|除以|除|\/)\s*([0-9\.\u4e00-\u9fa5]+)"
    match = re.search(pattern, t)
    if not match:
        return None
    
    s_a, s_op, s_b = match.group(1), match.group(2), match.group(3)
    a = chinese_to_arabic(s_a)
    b = chinese_to_arabic(s_b)
    
    if a is None or b is None:
        return None
    
    if s_op in ["+", "加", "加上"]:
        op = "+"
        ans = a + b
        op_name = "加法代数微柱"
    elif s_op in ["-", "减", "减去"]:
        op = "-"
        ans = a - b
        op_name = "差动比较微柱"
    elif s_op in ["*", "x", "X", "乘", "乘以", "乘上"]:
        op = "*"
        ans = a * b
        op_name = "非线性乘积微柱"
    elif s_op in ["/", "除", "除以"]:
        op = "/"
        ans = a / b if b != 0 else float('nan')
        op_name = "有理分式斜率微柱"
    else:
        return None
        
    return a, op, b, ans, op_name

if __name__ == "__main__":
    test_cases = ["五除2等于多少", "17加9是多少", "一百减去二十五", "三乘以四", "5/2", "128 * 4", "两百加五十"]
    for tc in test_cases:
        res = parse_natural_arithmetic(tc)
        print(f"'{tc}' -> {res}")
