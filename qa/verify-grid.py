#!/usr/bin/env python3
"""Independently check the edited workbook exported by the Warp guest."""
from argparse import ArgumentParser
from pathlib import Path
from zipfile import ZipFile

from openpyxl import load_workbook


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--price', type=int, default=28)
    args = parser.parse_args()

    with ZipFile(args.output) as archive:
        assert archive.testzip() is None, 'bad XLSX ZIP CRC'
        assert any(item.startswith('xl/charts/chart') and item.endswith('.xml')
                   for item in archive.namelist()), 'chart part missing'

    formulas = load_workbook(args.output)
    assert formulas.sheetnames == ['Forecast', 'Sales trend', 'Notes'], formulas.sheetnames
    assert formulas['Forecast']['B4'].value == '=20+22'
    assert formulas['Forecast']['D4'].value == '=B4*C4'
    assert formulas['Forecast']['A1'].value == 'NORTHSTAR STUDIO   /   PILOT FORECAST'
    assert 'A1:H1' in formulas['Forecast'].merged_cells
    assert len(formulas['Sales trend']._charts) == 1
    formulas.close()

    values = load_workbook(args.output, read_only=True, data_only=True)
    assert values['Forecast']['B4'].value == 42
    assert values['Forecast']['C4'].value == args.price
    assert values['Forecast']['D4'].value == 42 * args.price
    values.close()
    print('Verified XLSX CRCs, three sheets, edited and recalculated formulas, merged header, and chart.')


if __name__ == '__main__':
    main()
