#ifndef _HEAD_H
#define _HEAD_H

typedef struct desc_struct {
	unsigned long a,b;
} desc_table[256];

extern unsigned long pg_dir[1024];
extern desc_table idt,gdt;  // �ж���������ȫ����������

#define GDT_NUL 0		// ȫ����������ĵ� 0 ���ʹ��
#define GDT_CODE 1		// �� 1 �� -- �ں˵Ĵ������������
#define GDT_DATA 2		// �� 2 �� -- �ں˵����ݶ���������
#define GDT_TMP 3		// �� 3 �� -- ϵͳ�������linux û��ʹ��

#define LDT_NUL 0		// ÿ���ֲ���������ĵ� 0 ���ʹ��
#define LDT_CODE 1		// �� 1 �� -- �û�����Ĵ������������
#define LDT_DATA 2		// �� 2 �� -- �û���������ݶ���������

#endif
