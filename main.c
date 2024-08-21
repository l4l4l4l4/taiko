#include <avr/io.h>
#include <util/delay.h>

//#define F_CPU 16000000UL  // defined in makefile 
#define LED 13    // LED connected to PD7 (13)

int main(void)
{
    DDRD = (1<<PD7);

    while (1)
    {
	PORTD |= (1<<PD7);
        _delay_ms(500); 
	PORTD &= ~(1<<PD7);
        _delay_ms(500); 
    }
    return 0;
}

