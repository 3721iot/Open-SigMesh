2021/11/16
1���Ż��ײ��ڴ����ģ�
2���Ż��ײ����ݰ��ۻ�ʱ�л����ٷ�������

2021/06/23
1���Ż�mesh_stop
2���Ż�mesh�ײ����
3���Ż�app_mesh_send_user_adv_packet�ӿڹ���

2021/06/02
1������IV update�ӿ�
2��Ĭ��ʹ������������������secure beacon
3��֧��Ӧ�ò��ٿ�һ����ͨ�㲥��������ͨ���ݴ��䣬mac��ַ��mesh�㲥��ַ���ֿ��ˣ���ͨgatt�ص��ο�proj_main.c
�е�gatt�ص�������Ҫ�����Ӻŵ���Ϣ�Ĵ���

1. ������ӡ����DEV_KEY��NETWORK_KEY��APP_KEY
2. ��proj_main�ļ��е�user_entry_after_ble_init����������Ե���system_set_tx_power(RF_TX_POWER_NEG_16dBm);�����书�����õ���ͣ�ͬʱ�Ͽ��������ߣ���������provisonee���ź�ǿ�ȣ��Ӷ�ʵ�ֽ��������PB-Remote���ܡ�
3. ��provisonee��Ϊ�ӻ���provioner������֮����ӡ
    slave[0],connect. link_num:1
    0x81,0x5D,0x54,0x42,0x5F,0x62,
	ͨ����ӡ�ĵ�ַ���ж�������echo���������ڵ㣬�Ӷ����������Ƿ�ͨ��PB-Remote��